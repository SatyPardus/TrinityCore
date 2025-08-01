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

#ifndef __MASTERSESSION_H__
#define __MASTERSESSION_H__

#include <boost/asio/ip/tcp.hpp>
#include "AsyncCallbackProcessor.h"
#include "Common.h"
#include "CryptoHash.h"
#include "DatabaseEnvFwd.h"
#include "Duration.h"
#include "MPSCQueue.h"
#include "Optional.h"
#include "Socket.h"
#include "MasterServerOpcodes.h"
#include "MasterServerPacket.h"
#include "MasterSharedDefines.h"

using boost::asio::ip::tcp;

class ByteBuffer;

class MasterSession : public Socket<MasterSession>
{
    typedef Socket<MasterSession> MasterSocket;

  public:
    MasterSession(tcp::socket&& socket);

    void Start() override;
    bool Update() override;

  protected:
    void ReadHandler() override;

    bool ReadHeaderHandler();
    void OnPacketReceived(MasterServerOpcodes opcode, MasterServerPacket& packet);

  private:
    void HandleAuthSession(MasterServerPacket& packet);

    MessageBuffer _headerBuffer;
    MessageBuffer _packetBuffer;

    ClientType _clientType;
};

#endif
