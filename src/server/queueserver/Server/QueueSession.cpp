/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "QueueSession.h"
#include "AES.h"
#include "ByteBuffer.h"
#include "ClientBuildInfo.h"
#include "Config.h"
#include "CryptoGenerics.h"
#include "CryptoHash.h"
#include "CryptoRandom.h"
#include "DatabaseEnv.h"
#include "IPLocation.h"
#include "Log.h"
#include "RealmList.h"
#include "SecretMgr.h"
#include "TOTP.h"
#include "Util.h"
#include <boost/lexical_cast.hpp>
#include <SharedDefines.h>
#include <HMAC.h>

using boost::asio::ip::tcp;

QueueSession::QueueSession(tcp::socket&& socket)
: Socket(std::move(socket)), _authed(false), _sendBufferSize(4096), _expansion(0), _queuePosition(0), _account()
{
    Trinity::Crypto::GetRandomBytes(_authSeed);
    _headerBuffer.Resize(sizeof(ClientPktHeader));
}

void QueueSession::Start()
{
    std::string ip_address = GetRemoteIpAddress().to_string();
    TC_LOG_ERROR("session", "Accepted connection from {}", ip_address);

    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_IP_INFO);
    stmt->setString(0, ip_address);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(
        std::bind(&QueueSession::CheckIpCallback, this, std::placeholders::_1)));
}

bool QueueSession::Update()
{
    using namespace std::chrono;

    if (_authed)
    {
        if (_LastPingTime == steady_clock::time_point())
        {
            _LastPingTime = steady_clock::now();
        }
        else
        {
            steady_clock::time_point now = steady_clock::now();

            steady_clock::duration diff = now - _LastPingTime;

            if (diff >= seconds(1))
            {
                _LastPingTime = now;
                if (_queuePosition > 0)
                {
                    _queuePosition--;
                    TC_LOG_ERROR("session", "Queue position {}", _queuePosition);
                    SendAuthWaitQueue(_queuePosition);
                }
            }
        }
    }

    EncryptablePacket* queued;
    if (_bufferQueue.Dequeue(queued))
    {
        // Allocate buffer only when it's needed but not on every Update() call.
        MessageBuffer buffer(_sendBufferSize);
        do
        {
            ServerPktHeader header(queued->size() + 2, queued->GetOpcode());
            if (queued->NeedsEncryption())
                _authCrypt.EncryptSend(header.header, header.getHeaderLength());

            if (buffer.GetRemainingSpace() < queued->size() + header.getHeaderLength())
            {
                QueuePacket(std::move(buffer));
                buffer.Resize(_sendBufferSize);
            }

            if (buffer.GetRemainingSpace() >= queued->size() + header.getHeaderLength())
            {
                buffer.Write(header.header, header.getHeaderLength());
                if (!queued->empty())
                    buffer.Write(queued->contents(), queued->size());
            }
            else // single packet larger than buffer size
            {
                MessageBuffer packetBuffer(queued->size() + header.getHeaderLength());
                packetBuffer.Write(header.header, header.getHeaderLength());
                if (!queued->empty())
                    packetBuffer.Write(queued->contents(), queued->size());

                QueuePacket(std::move(packetBuffer));
            }

            delete queued;
        } while (_bufferQueue.Dequeue(queued));

        if (buffer.GetActiveSize() > 0)
            QueuePacket(std::move(buffer));
    }

    if (!QueueSocket::Update())
        return false;

    _queryProcessor.ProcessReadyCallbacks();

    return true;
}

void QueueSession::ReadHandler()
{
    MessageBuffer& packet = GetReadBuffer();
    while (packet.GetActiveSize() > 0)
    {
        if (_headerBuffer.GetRemainingSpace() > 0)
        {
            // need to receive the header
            std::size_t readHeaderSize = std::min(packet.GetActiveSize(), _headerBuffer.GetRemainingSpace());
            _headerBuffer.Write(packet.GetReadPointer(), readHeaderSize);
            packet.ReadCompleted(readHeaderSize);

            if (_headerBuffer.GetRemainingSpace() > 0)
            {
                // Couldn't receive the whole header this time.
                ASSERT(packet.GetActiveSize() == 0);
                break;
            }

            // We just received nice new header
            if (!ReadHeaderHandler())
            {
                CloseSocket();
                return;
            }
        }

        // We have full read header, now check the data payload
        if (_packetBuffer.GetRemainingSpace() > 0)
        {
            // need more data in the payload
            std::size_t readDataSize = std::min(packet.GetActiveSize(), _packetBuffer.GetRemainingSpace());
            _packetBuffer.Write(packet.GetReadPointer(), readDataSize);
            packet.ReadCompleted(readDataSize);

            if (_packetBuffer.GetRemainingSpace() > 0)
            {
                // Couldn't receive the whole data this time.
                ASSERT(packet.GetActiveSize() == 0);
                break;
            }
        }

        // just received fresh new payload
        ReadDataHandlerResult result = ReadDataHandler();
        _headerBuffer.Reset();
        if (result != ReadDataHandlerResult::Ok)
        {
            if (result != ReadDataHandlerResult::WaitingForQuery)
                CloseSocket();

            return;
        }
    }

    AsyncRead();
}

bool QueueSession::ReadHeaderHandler()
{
    ASSERT(_headerBuffer.GetActiveSize() == sizeof(ClientPktHeader));

    if (_authCrypt.IsInitialized())
        _authCrypt.DecryptRecv(_headerBuffer.GetReadPointer(), sizeof(ClientPktHeader));

    ClientPktHeader* header = reinterpret_cast<ClientPktHeader*>(_headerBuffer.GetReadPointer());
    EndianConvertReverse(header->size);
    EndianConvert(header->cmd);

    if (!(header->size >= 4 && header->size < 10240) || !header->IsValidOpcode())
    {
        TC_LOG_ERROR("network", "QueueSession::ReadHeaderHandler(): client {} sent malformed packet (size: {}, cmd: {})",
                     GetRemoteIpAddress().to_string(), header->size, header->cmd);
        return false;
    }

    header->size -= sizeof(header->cmd);
    _packetBuffer.Resize(header->size);
    return true;
}

QueueSession::ReadDataHandlerResult QueueSession::ReadDataHandler()
{
    ClientPktHeader* header = reinterpret_cast<ClientPktHeader*>(_headerBuffer.GetReadPointer());
    Opcodes opcode          = static_cast<Opcodes>(header->cmd);

    WorldPacket packet(opcode, std::move(_packetBuffer));

    switch (opcode)
    {
        case CMSG_PING:
        {
            try
            {
                return HandlePing(packet) ? ReadDataHandlerResult::Ok : ReadDataHandlerResult::Error;
            }
            catch (ByteBufferException const&)
            {
            }
            TC_LOG_ERROR("network", "WorldSocket::ReadDataHandler(): client {} sent malformed CMSG_PING",
                         GetRemoteIpAddress().to_string());
            return ReadDataHandlerResult::Error;
        }
        case CMSG_AUTH_SESSION:
        {
            if (_authed)
            {
                TC_LOG_ERROR("network", "WorldSocket::ProcessIncoming: received duplicate CMSG_AUTH_SESSION from {}",
                             GetRemoteIpAddress().to_string());
                return ReadDataHandlerResult::Error;
            }

            try
            {
                HandleAuthSession(packet);
                return ReadDataHandlerResult::WaitingForQuery;
            }
            catch (ByteBufferException const&)
            {
            }
            TC_LOG_ERROR("network", "WorldSocket::ReadDataHandler(): client {} sent malformed CMSG_AUTH_SESSION",
                         GetRemoteIpAddress().to_string());
            return ReadDataHandlerResult::Error;
        }
        case CMSG_KEEP_ALIVE: // todo: handle this packet in the same way of CMSG_TIME_SYNC_RESP
            return ReadDataHandlerResult::Ok;
        case CMSG_SUSPEND_COMMS_ACK:
            return ReadDataHandlerResult::Ok;
    }

    TC_LOG_ERROR("network.opcode", "ProcessIncoming: Client not authed opcode = {}", uint32(opcode));
    return ReadDataHandlerResult::Error;
}

void QueueSession::CheckIpCallback(PreparedQueryResult result)
{
    if (result)
    {
        bool banned = false;
        do
        {
            Field* fields = result->Fetch();
            if (fields[0].GetUInt64() != 0)
                banned = true;

        } while (result->NextRow());

        if (banned)
        {
            SendAuthResponseError(AUTH_REJECT);
            TC_LOG_ERROR("network", "WorldSocket::CheckIpCallback: Sent Auth Response (IP {} banned).",
                         GetRemoteIpAddress().to_string());
            DelayedCloseSocket();
            return;
        }
    }

    AsyncRead();
    HandleSendAuthSession();
}

void QueueSession::HandleSendAuthSession()
{
    WorldPacket packet(SMSG_AUTH_CHALLENGE, 40);
    packet << uint32(1); // 1...31
    packet.append(_authSeed);

    packet.append(Trinity::Crypto::GetRandomBytes<32>()); // new encryption seeds

    SendPacketAndLogOpcode(packet);
}

void QueueSession::HandleAuthSession(WorldPacket& recvPacket)
{
    std::shared_ptr<AuthSession> authSession = std::make_shared<AuthSession>();

    // Read the content of the packet
    recvPacket >> authSession->Build;
    recvPacket >> authSession->LoginServerID;
    recvPacket >> authSession->Account;
    recvPacket >> authSession->LoginServerType;
    recvPacket.read(authSession->LocalChallenge);
    recvPacket >> authSession->RegionID;
    recvPacket >> authSession->BattlegroupID;
    recvPacket >> authSession->RealmID; // realmId from auth_database.realmlist table
    recvPacket >> authSession->DosResponse;
    recvPacket.read(authSession->Digest);
    authSession->AddonInfo.resize(recvPacket.size() - recvPacket.rpos());
    recvPacket.read(authSession->AddonInfo.contents(),
                    authSession->AddonInfo.size()); // .contents will throw if empty, thats what we want

    // Get the account information from the auth database
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_INFO_BY_NAME);
    stmt->setInt32(0, int32(realm.Id.Realm));
    stmt->setString(1, authSession->Account);

    _queryProcessor.AddCallback(LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(
        [this, authSession = std::move(authSession)](PreparedQueryResult result) mutable
        { HandleAuthSessionCallback(std::move(authSession), std::move(result)); }));
}

void QueueSession::HandleAuthSessionCallback(std::shared_ptr<AuthSession> authSession, PreparedQueryResult result)
{
    // Stop if the account is not found
    if (!result)
    {
        // We can not log here, as we do not know the account. Thus, no accountId.
        SendAuthResponseError(AUTH_UNKNOWN_ACCOUNT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (unknown account).");
        DelayedCloseSocket();
        return;
    }

    AccountInfo account(result->Fetch());

    // For hook purposes, we get Remoteaddress at this point.
    std::string address = GetRemoteIpAddress().to_string();

    LoginDatabasePreparedStatement* stmt = nullptr;

    // even if auth credentials are bad, try using the session key we have - client cannot read auth response error
    // without it
    _authCrypt.Init(account.SessionKey);

    // First reject the connection if packet contains invalid data or realm state doesn't allow logging in
    // TODO should we actually close when the "real" world is closed?
    // or just keep players in a queue? Maybe a config...
    /*if (sWorld->IsClosed())
    {
        SendAuthResponseError(AUTH_REJECT);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: World closed, denying client ({}).",
                     GetRemoteIpAddress().to_string());
        DelayedCloseSocket();
        return;
    }*/

    if (authSession->RealmID != realm.Id.Realm)
    {
        SendAuthResponseError(REALM_LIST_REALM_NOT_FOUND);
        TC_LOG_ERROR("network",
                     "WorldSocket::HandleAuthSession: Client {} requested connecting with realm id {} but this realm "
                     "has id {} set in config.",
                     GetRemoteIpAddress().to_string(), authSession->RealmID, realm.Id.Realm);
        DelayedCloseSocket();
        return;
    }

    // Check that Key and account name are the same on client and server
    uint8 t[4] = {0x00, 0x00, 0x00, 0x00};

    Trinity::Crypto::SHA1 sha;
    sha.UpdateData(authSession->Account);
    sha.UpdateData(t);
    sha.UpdateData(authSession->LocalChallenge);
    sha.UpdateData(_authSeed);
    sha.UpdateData(account.SessionKey);
    sha.Finalize();

    if (sha.GetDigest() != authSession->Digest)
    {
        SendAuthResponseError(AUTH_FAILED);
        TC_LOG_ERROR("network",
                     "WorldSocket::HandleAuthSession: Authentication failed for account: {} ('{}') address: {}",
                     account.Id, authSession->Account, address);
        DelayedCloseSocket();
        return;
    }

    if (IpLocationRecord const* location = sIPLocation->GetLocationRecord(address))
        _ipCountry = location->CountryCode;

    ///- Re-check ip locking (same check as in auth).
    if (account.IsLockedToIP)
    {
        if (account.LastIP != address)
        {
            SendAuthResponseError(AUTH_FAILED);
            TC_LOG_DEBUG(
                "network",
                "WorldSocket::HandleAuthSession: Sent Auth Response (Account IP differs. Original IP: {}, new IP: {}).",
                account.LastIP, address);
            // We could log on hook only instead of an additional db log, however action logger is config based. Better
            // keep DB logging as well
            //sScriptMgr->OnFailedAccountLogin(account.Id);
            DelayedCloseSocket();
            return;
        }
    }
    else if (!account.LockCountry.empty() && account.LockCountry != "00" && !_ipCountry.empty())
    {
        if (account.LockCountry != _ipCountry)
        {
            SendAuthResponseError(AUTH_FAILED);
            TC_LOG_DEBUG("network",
                         "WorldSocket::HandleAuthSession: Sent Auth Response (Account country differs. Original "
                         "country: {}, new country: {}).",
                         account.LockCountry, _ipCountry);
            // We could log on hook only instead of an additional db log, however action logger is config based. Better
            // keep DB logging as well
            //sScriptMgr->OnFailedAccountLogin(account.Id);
            DelayedCloseSocket();
            return;
        }
    }

    if (account.IsBanned)
    {
        SendAuthResponseError(AUTH_BANNED);
        TC_LOG_ERROR("network", "WorldSocket::HandleAuthSession: Sent Auth Response (Account banned).");
        //sScriptMgr->OnFailedAccountLogin(account.Id);
        DelayedCloseSocket();
        return;
    }

    // Check locked state for server
    AccountTypes allowedAccountType = SEC_PLAYER; // TODO sWorld->GetPlayerSecurityLimit();
    TC_LOG_DEBUG("network", "Allowed Level: {} Player Level {}", allowedAccountType, account.Security);
    if (allowedAccountType > SEC_PLAYER && account.Security < allowedAccountType)
    {
        SendAuthResponseError(AUTH_UNAVAILABLE);
        TC_LOG_DEBUG("network",
                     "WorldSocket::HandleAuthSession: User tries to login but his security level is not enough");
       // sScriptMgr->OnFailedAccountLogin(account.Id);
        DelayedCloseSocket();
        return;
    }

    TC_LOG_DEBUG("network", "WorldSocket::HandleAuthSession: Client '{}' authenticated successfully from {}.",
                 authSession->Account, address);

    _authed       = true;
    _queuePosition = 0;
    _account       = account;

    SendAuthWaitQueue(_queuePosition);

    AsyncRead();
}

bool QueueSession::HandlePing(WorldPacket& recvPacket)
{
    using namespace std::chrono;

    uint32 ping;
    uint32 latency;

    // Get the ping packet content
    recvPacket >> ping;
    recvPacket >> latency;

    

    WorldPacket packet(SMSG_PONG, 4);
    packet << ping;
    SendPacketAndLogOpcode(packet);
    return true;
}

void QueueSession::SendAuthWaitQueue(uint32 position)
{
    if (position == 0)
    {
        auto addr = boost::asio::ip::make_address_v4("127.0.0.1").to_bytes();
        WorldPacket pkt(SMSG_REDIRECT_CLIENT, 4 + 2 + 4 + 20);

        uint16 port = 8085;

        // pkt << ip2;                                     // inet_addr(ipstr)
        pkt.append(addr.data(), 4);
        pkt << uint16(port); // port

        pkt << uint32(0); // token

        Trinity::Crypto::HMAC_SHA1 sha1(_account.SessionKey.data(), 40);
        // sha1.UpdateData((uint8*)&ip2, 4);
        sha1.UpdateData(addr.data(), 4);
        sha1.UpdateData((uint8*)&port, 2);
        sha1.Finalize();
        pkt.append(sha1.GetDigest()); // hmacsha1(ip+port) w/ sessionkey as seed

        SendPacket(pkt);

        WorldPacket packet(SMSG_SUSPEND_COMMS, 6);
        packet << uint32(0);
        SendPacket(packet);
    }
    else
    {
        WorldPacket packet(SMSG_AUTH_RESPONSE, 6);
        packet << uint8(AUTH_WAIT_QUEUE);
        packet << uint32(position);
        packet << uint8(0); // unk
        SendPacket(packet);
    }
}

void QueueSession::SendAuthResponse(uint8 code, bool shortForm, uint32 queuePos)
{
    WorldPacket packet(SMSG_AUTH_RESPONSE, 1 + 4 + 1 + 4 + 1 + (4 + 1));
    packet << uint8(code);
    packet << uint32(0);          // BillingTimeRemaining
    packet << uint8(0);           // BillingPlanFlags
    packet << uint32(0);          // BillingTimeRested
    packet << uint8(_expansion); // 0 - normal, 1 - TBC, 2 - WOTLK, must be set in database manually for each account

    if (!shortForm)
    {
        packet << uint32(queuePos); // Queue position
        packet << uint8(0);         // Realm has a free character migration - bool
    }

    SendPacket(packet);
}

void QueueSession::SendAuthResponseError(uint8 code)
{
    WorldPacket packet(SMSG_AUTH_RESPONSE, 1);
    packet << uint8(code);

    SendPacketAndLogOpcode(packet);
}

void QueueSession::SendPacketAndLogOpcode(WorldPacket const& packet)
{
    // TODO
    /*TC_LOG_TRACE("network.opcode", "S->C: {} {}", GetRemoteIpAddress().to_string(),
                 GetOpcodeNameForLogging(static_cast<Opcodes>(packet.GetOpcode())));*/
    SendPacket(packet);
}

void QueueSession::SendPacket(WorldPacket const& packet)
{
    if (!IsOpen())
        return;

    _bufferQueue.Enqueue(new EncryptablePacket(packet, _authCrypt.IsInitialized()));
}
