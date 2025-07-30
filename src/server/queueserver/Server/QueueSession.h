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

#ifndef __QUEUESESSION_H__
#define __QUEUESESSION_H__

#include "AsyncCallbackProcessor.h"
#include "Common.h"
#include "CryptoHash.h"
#include "DatabaseEnvFwd.h"
#include "Duration.h"
#include "Optional.h"
#include "Socket.h"
#include "SRP6.h"
#include <boost/asio/ip/tcp.hpp>
#include <QueueWorldPacket.h>
#include <AuthCrypt.h>
#include "MPSCQueue.h"
#include "QueueOpcodes.h"

using boost::asio::ip::tcp;
class EncryptablePacket : public WorldPacket
{
  public:
    EncryptablePacket(WorldPacket const& packet, bool encrypt) : WorldPacket(packet), _encrypt(encrypt)
    {
        SocketQueueLink.store(nullptr, std::memory_order_relaxed);
    }

    bool NeedsEncryption() const
    {
        return _encrypt;
    }

    std::atomic<EncryptablePacket*> SocketQueueLink;

  private:
    bool _encrypt;
};

#pragma pack(push, 1)

struct ClientPktHeader
{
    uint16 size;
    uint32 cmd;

    // @tswow-begin move implementation to cpp
    bool IsValidSize() const;
    // @tswow-end
    bool IsValidOpcode() const
    {
        return cmd < NUM_OPCODE_HANDLERS;
    }
};

#pragma pack(pop)

#pragma pack(push, 1)

struct ServerPktHeader
{
    /**
     * size is the length of the payload _plus_ the length of the opcode
     */
    ServerPktHeader(uint32 size, uint16 cmd) : size(size)
    {
        uint8 headerIndex = 0;
        if (isLargePacket())
        {
            TC_LOG_DEBUG("network", "initializing large server to client packet. Size: {}, cmd: {}", size, cmd);
            header[headerIndex++] = 0x80 | (0xFF & (size >> 16));
        }
        header[headerIndex++] = 0xFF & (size >> 8);
        header[headerIndex++] = 0xFF & size;

        header[headerIndex++] = 0xFF & cmd;
        header[headerIndex++] = 0xFF & (cmd >> 8);
    }

    uint8 getHeaderLength()
    {
        // cmd = 2 bytes, size= 2||3bytes
        return 2 + (isLargePacket() ? 3 : 2);
    }

    bool isLargePacket() const
    {
        return size > 0x7FFF;
    }

    const uint32 size;
    uint8 header[5];
};

#pragma pack(pop)

struct AuthSession
{
    uint32 BattlegroupID                 = 0;
    uint32 LoginServerType               = 0;
    uint32 RealmID                       = 0;
    uint32 Build                         = 0;
    std::array<uint8, 4> LocalChallenge  = {};
    uint32 LoginServerID                 = 0;
    uint32 RegionID                      = 0;
    uint64 DosResponse                   = 0;
    Trinity::Crypto::SHA1::Digest Digest = {};
    std::string Account;
    ByteBuffer AddonInfo;
};

struct AccountInfo
{
    uint32 Id;
    ::SessionKey SessionKey;
    std::string LastIP;
    bool IsLockedToIP;
    std::string LockCountry;
    uint8 Expansion;
    int64 MuteTime;
    LocaleConstant Locale;
    uint32 Recruiter;
    std::string OS;
    Minutes TimezoneOffset;
    bool IsRectuiter;
    AccountTypes Security;
    bool IsBanned;

    explicit AccountInfo()
    {

    }

    explicit AccountInfo(Field const* fields)
    {
        //           0             1          2         3               4            5           6         7 8     9 10
        //           11
        // SELECT a.id, a.sessionkey, a.last_ip, a.locked, a.lock_country, a.expansion, a.mutetime, a.locale,
        // a.recruiter, a.os, a.timezone_offset, aa.SecurityLevel,
        //                                                           12    13
        // ab.unbandate > UNIX_TIMESTAMP() OR ab.unbandate = ab.bandate, r.id
        // FROM account a
        // LEFT JOIN account_access aa ON a.id = aa.AccountID AND aa.RealmID IN (-1, ?)
        // LEFT JOIN account_banned ab ON a.id = ab.id
        // LEFT JOIN account r ON a.id = r.recruiter
        // WHERE a.username = ? ORDER BY aa.RealmID DESC LIMIT 1
        Id             = fields[0].GetUInt32();
        SessionKey     = fields[1].GetBinary<SESSION_KEY_LENGTH>();
        LastIP         = fields[2].GetString();
        IsLockedToIP   = fields[3].GetBool();
        LockCountry    = fields[4].GetString();
        Expansion      = fields[5].GetUInt8();
        MuteTime       = fields[6].GetInt64();
        Locale         = LocaleConstant(fields[7].GetUInt8());
        Recruiter      = fields[8].GetUInt32();
        OS             = fields[9].GetString();
        TimezoneOffset = Minutes(fields[10].GetInt16());
        Security       = AccountTypes(fields[11].GetUInt8());
        IsBanned       = fields[12].GetUInt64() != 0;
        IsRectuiter    = fields[13].GetUInt32() != 0;

        uint32 world_expansion = 3; // TODO
        if (Expansion > world_expansion)
            Expansion = world_expansion;

        if (Locale >= TOTAL_LOCALES)
            Locale = LOCALE_enUS;
    }
};

class ByteBuffer;

class QueueSession : public Socket<QueueSession>
{
    typedef Socket<QueueSession> QueueSocket;

public:
    QueueSession(tcp::socket&& socket);

    void Start() override;
    bool Update() override;

protected:
    void ReadHandler() override;

    bool ReadHeaderHandler();

    enum class ReadDataHandlerResult
    {
        Ok              = 0,
        Error           = 1,
        WaitingForQuery = 2
    };

    ReadDataHandlerResult ReadDataHandler();

private:
    void CheckIpCallback(PreparedQueryResult result);
    void HandleSendAuthSession();
    void HandleAuthSession(WorldPacket& recvPacket);
    void HandleAuthSessionCallback(std::shared_ptr<AuthSession> authSession, PreparedQueryResult result);
    bool HandlePing(WorldPacket& recvPacket);

    void SendPacket(WorldPacket const& packet);
    void SendPacketAndLogOpcode(WorldPacket const& packet);
    void SendAuthResponse(uint8 code, bool shortForm, uint32 queuePos = 0);
    void SendAuthResponseError(uint8 code);
    void SendAuthWaitQueue(uint32 position);

    uint32 _queuePosition;
    uint8 _expansion;
    bool _authed;
    TimePoint _LastPingTime;
    AccountInfo _account;

    MessageBuffer _headerBuffer;
    MessageBuffer _packetBuffer;

    std::array<uint8, 4> _authSeed;
    AuthCrypt _authCrypt;

    MPSCQueue<EncryptablePacket, &EncryptablePacket::SocketQueueLink> _bufferQueue;
    std::size_t _sendBufferSize;

    QueryCallbackProcessor _queryProcessor;
    std::string _ipCountry;
};

#endif
