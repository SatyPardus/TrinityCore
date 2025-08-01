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

#include "Common.h"
#include "WorldMasterServerHandler.h"
#include "IoContext.h"
#include "MasterServerPacket.h"
#include "MasterSharedDefines.h"
#include "World.h"
#include "Realm.h"

std::shared_ptr<WorldMasterServerHandler> WorldMasterServerHandler::_instance = nullptr;

WorldMasterServerHandler::WorldMasterServerHandler(Trinity::Asio::IoContext& ioContext) : MasterServerSocket(ioContext)
{
}

std::shared_ptr<WorldMasterServerHandler> WorldMasterServerHandler::instance()
{
    return WorldMasterServerHandler::_instance;
}

void WorldMasterServerHandler::OnConnected() {
    MasterServerPacket authPacket(MASTER_MSG_AUTHENTICATE, 1);
    authPacket << (uint8)CLIENT_TYPE_WORLD;
    authPacket << realm.Id.Realm;
    SendPacket(authPacket);

    TC_LOG_INFO("session", "Connected to master server!");
}

void WorldMasterServerHandler::OnDisconnected() {
    
}

void WorldMasterServerHandler::OnPacketReceived(MasterServerOpcodes opcode, MasterServerPacket& packet)
{
    if (opcode == MASTER_MSG_REQUEST_OPEN_SLOTS)
    {
        MasterServerPacket ackPacket = MasterServerPacket(MASTER_MSG_REQUEST_OPEN_SLOTS_ACK, 4);
        if (sWorld->GetPlayerCount() >= sWorld->GetPlayerAmountLimit())
        {
            ackPacket << uint32(sWorld->GetPlayerAmountLimit() ? 0 : 100);
        }
        else
        {
            ackPacket << uint32(sWorld->GetPlayerAmountLimit() - sWorld->GetPlayerCount());
        }
        SendPacket(ackPacket);
    }
    else
    {
        TC_LOG_ERROR("session", "Received unhandled opcode: {}", uint32(opcode));
    }
}
