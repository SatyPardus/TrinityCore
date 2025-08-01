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

#ifndef MASTERSERVERCLIENT_H
#define MASTERSERVERCLIENT_H

#include "ClientSocket.h"
#include "IoContext.h"
#include "DeadlineTimer.h"
#include "MasterServerPacket.h"
#include "MasterServerOpcodes.h"

#pragma pack(push, 1)

struct MasterPktHeader
{
    uint16 size;
    uint16 cmd;

    bool IsValidOpcode() const
    {
        return cmd > MASTER_MSG_NONE && cmd < NUM_MASTER_MSG_TYPES;
    }
};

#pragma pack(pop)

class TC_SHARED_API MasterServerClient : public ClientSocket<MasterServerClient>
{
    typedef ClientSocket<MasterServerClient> MasterServerSocket;

public:
    MasterServerClient(Trinity::Asio::IoContext& ioContext);
    ~MasterServerClient();

    void Start() override;
    bool Update() override;
    void SendPacket(MasterServerPacket const& packet);

protected:
    void Run();
    void UpdateSocket();
    void OnClose() override;
    void ReadHandler() override;
    bool ReadHeaderHandler();

    virtual void OnConnected() = 0;
    virtual void OnDisconnected() = 0;
    virtual void OnPacketReceived(MasterServerOpcodes opcode, MasterServerPacket const& packet) = 0;

    Trinity::Asio::IoContext _ioContext;
    std::thread* _updateThread;
    Trinity::Asio::DeadlineTimer _updateTimer;
    std::atomic<bool> _stopped;
    std::atomic<bool> _connected;
    std::atomic<bool> _connecting;
    uint8 _packetHeader[4];

    MessageBuffer _headerBuffer;
    MessageBuffer _packetBuffer;
};

#endif
