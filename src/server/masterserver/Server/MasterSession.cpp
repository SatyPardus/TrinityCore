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

using boost::asio::ip::tcp;

MasterSession::MasterSession(tcp::socket&& socket)
: Socket(std::move(socket))
{
    _headerBuffer.Resize(sizeof(MasterPktHeader));
}

void MasterSession::Start()
{
    std::string ip_address = GetRemoteIpAddress().to_string();
    TC_LOG_ERROR("session", "Accepted connection from {}", ip_address);
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

    if (!(header->size >= 4 && header->size < 10240) || !header->IsValidOpcode())
    {
        TC_LOG_ERROR("network",
                     "MasterSession::ReadHeaderHandler(): client {} sent malformed packet (size: {}, cmd: {})",
                     GetRemoteIpAddress().to_string(), header->size, header->cmd);
        return false;
    }

    header->size -= sizeof(header->cmd);
    _packetBuffer.Resize(header->size);
    return true;
}

void MasterSession::OnPacketReceived(MasterServerOpcodes opcode, MasterServerPacket const& packet) {

}
