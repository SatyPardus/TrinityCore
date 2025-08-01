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

#include <HMAC.h>
#include <SharedDefines.h>
#include <boost/lexical_cast.hpp>
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
#include "MasterSession.h"
#include "RealmList.h"
#include "SecretMgr.h"
#include "TOTP.h"
#include "Util.h"
#include "MasterPktHeader.h"
#include <MasterServerPacket.h>
#include "MasterMgr.h"

using boost::asio::ip::tcp;

MasterSession::MasterSession(tcp::socket&& socket)
: Socket(std::move(socket)), _clientType(CLIENT_TYPE_NONE), _clientID(0), _packetHeader()
{
    _headerBuffer.Resize(sizeof(MasterPktHeader));
}

ClientType MasterSession::GetType()
{
    return _clientType;
}

uint32 MasterSession::GetID()
{
    return _clientID;
}

void MasterSession::Start()
{
    std::string ip_address = GetRemoteIpAddress().to_string();
    TC_LOG_INFO("session", "Accepted connection from {}", ip_address);

    AsyncRead();

    sMaster->AddSession(this);
}

void MasterSession::OnClose()
{
    sMaster->RemoveSession(this);
}

bool MasterSession::Update()
{
    if (!MasterSocket::Update())
        return false;

    return true;
}

void MasterSession::ReadHandler()
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
        MasterPktHeader* header    = reinterpret_cast<MasterPktHeader*>(_headerBuffer.GetReadPointer());
        MasterServerOpcodes opcode = static_cast<MasterServerOpcodes>(header->cmd);

        MasterServerPacket packet(opcode, std::move(_packetBuffer));
        OnPacketReceived(opcode, packet);
        _headerBuffer.Reset();
    }

    AsyncRead();
}

bool MasterSession::ReadHeaderHandler()
{
    ASSERT(_headerBuffer.GetActiveSize() == sizeof(MasterPktHeader));

    MasterPktHeader* header = reinterpret_cast<MasterPktHeader*>(_headerBuffer.GetReadPointer());
    EndianConvertReverse(header->size);
    EndianConvert(header->cmd);

    if (!(header->size >= 0 && header->size < 10240) || !header->IsValidOpcode())
    {
        TC_LOG_ERROR("network",
                     "MasterSession::ReadHeaderHandler(): client {} sent malformed packet (size: {}, cmd: {})",
                     GetRemoteIpAddress().to_string(), header->size, header->cmd);
        return false;
    }

    _packetBuffer.Resize(header->size);
    return true;
}

void MasterSession::OnPacketReceived(MasterServerOpcodes opcode, MasterServerPacket& packet) {
    switch (_clientType)
    {
        case CLIENT_TYPE_NONE:
        {
            if (opcode == MASTER_MSG_AUTHENTICATE)
            {
                try
                {
                    HandleAuthSession(packet);
                    return;
                }
                catch (ByteBufferException const&)
                {
                }
                TC_LOG_ERROR("network", "MasterSession::OnPacketReceived(): client {} sent malformed MASTER_MSG_AUTHENTICATE",
                             GetRemoteIpAddress().to_string());
                CloseSocket();
                return;
            }
            TC_LOG_ERROR("network.opcode", "MasterSession::OnPacketReceived(): Client not authed opcode = {}", uint32(opcode));
            CloseSocket();
            return;
        }
        case CLIENT_TYPE_WORLD:
        {
            if (opcode == MASTER_MSG_REQUEST_OPEN_SLOTS_ACK)
            {
                auto sessions           = sMaster->GetSessions();
                Sessions::iterator iter = sessions.begin();

                for (; iter != sessions.end(); ++iter)
                {
                    if ((*iter)->GetType() == CLIENT_TYPE_QUEUE && (*iter)->GetID() == this->GetID())
                    {
                        (*iter)->SendPacket(packet);
                    }
                }
                return;
            }
        }
        break;
        case CLIENT_TYPE_QUEUE:
        {
            if (opcode == MASTER_MSG_REQUEST_OPEN_SLOTS)
            {
                auto sessions           = sMaster->GetSessions();
                Sessions::iterator iter = sessions.begin();

                for (; iter != sessions.end(); ++iter)
                {
                    if ((*iter)->GetType() == CLIENT_TYPE_WORLD && (*iter)->GetID() == this->GetID())
                    {
                        (*iter)->SendPacket(packet);
                    }
                }
                return;
            }
        }
        break;
    }

    TC_LOG_ERROR("network.opcode", "MasterSession::OnPacketReceived(): Received unhandled opcode = {}", uint32(opcode));
}

void MasterSession::HandleAuthSession(MasterServerPacket& packet)
{
    packet >> reinterpret_cast<uint8_t&>(_clientType);

    if (_clientType == CLIENT_TYPE_AUTH)
    {
        TC_LOG_INFO("session", "Accepted AUTH server");
    }
    else if (_clientType == CLIENT_TYPE_WORLD)
    {
        packet >> _clientID;
        TC_LOG_INFO("session", "Accepted WORLD server with ID {}", _clientID);
    }
    else if (_clientType == CLIENT_TYPE_QUEUE)
    {
        packet >> _clientID;
        _clientID -= 1;
        TC_LOG_INFO("session", "Accepted QUEUE server with ID {}", _clientID);
    }
    else
    {
        TC_LOG_ERROR("network.opcode", "MasterSession::HandleAuthSession(): Client sent invalid client type = {}",
                     uint32(_clientType));
        CloseSocket();
        return;
    }
}

void MasterSession::SendPacket(MasterServerPacket& packet)
{
    if (!IsOpen())
        return;

    _packetHeader[0] = 0xFF & (packet.size() >> 8);
    _packetHeader[1] = 0xFF & packet.size();

    _packetHeader[2] = 0xFF & packet.GetOpcode();
    _packetHeader[3] = 0xFF & (packet.GetOpcode() >> 8);

    MessageBuffer buffer(packet.size() + 4);
    buffer.Write(_packetHeader, 4);
    if (packet.size())
    {
        buffer.Write(packet.contents(), packet.size());
    }
    QueuePacket(std::move(buffer));
}
