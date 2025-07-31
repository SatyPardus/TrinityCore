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

#ifndef QUEUE_MASTERSERVERHANDLER_H
#define QUEUE_MASTERSERVERHANDLER_H

#include "IoContext.h"
#include "MasterServerClient.h"
#include "MasterServerOpcodes.h"
#include "MasterServerPacket.h"

class MasterServerHandler : public MasterServerClient
{
    typedef MasterServerClient MasterServerSocket;

  public:
    MasterServerHandler(Trinity::Asio::IoContext& ioContext);

  protected:
    void OnPacketReceived(MasterServerOpcodes opcode, MasterServerPacket const& packet) override;
};

#endif
