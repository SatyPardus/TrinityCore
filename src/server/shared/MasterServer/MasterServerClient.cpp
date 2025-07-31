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

#include "MasterServerClient.h"
#include "IoContext.h"
#include "DeadlineTimer.h"
#include "MasterServerPacket.h"

MasterServerClient::MasterServerClient(Trinity::Asio::IoContext& ioContext)
: MasterServerSocket(ioContext), _stopped(false), _updateThread(nullptr), _ioContext(1), _updateTimer(_ioContext),
  _connected(false), _connecting(true), _packetHeader()
{
    _headerBuffer.Resize(sizeof(MasterPktHeader));
    _updateThread = new std::thread(&MasterServerClient::Run, this);
}

MasterServerClient::~MasterServerClient() {
    _stopped = true;
    _updateThread->join();
    _updateThread = nullptr;
}

void MasterServerClient::Run()
{
    TC_LOG_DEBUG("misc", "Network Thread Starting");

    _updateTimer.expires_from_now(boost::posix_time::milliseconds(100));
    _updateTimer.async_wait([this](boost::system::error_code const&) { UpdateSocket(); });
    _ioContext.run();

    TC_LOG_DEBUG("misc", "Network Thread exits");
}

void MasterServerClient::UpdateSocket()
{
    if (_stopped)
        return;

    if (!_connected.load() || !this->IsOpen())
    {
        if (!_connecting.load())
        {
            _connecting = true;
            this->Reconnect();
        }

        _updateTimer.expires_from_now(boost::posix_time::milliseconds(1000));
        _updateTimer.async_wait([this](boost::system::error_code const&) { UpdateSocket(); });
        return;
    }

    _updateTimer.expires_from_now(boost::posix_time::milliseconds(100));
    _updateTimer.async_wait([this](boost::system::error_code const&) { UpdateSocket(); });

    if (!this->Update())
    {
        if (this->IsOpen())
            this->CloseSocket();
    }
}

void MasterServerClient::Start()
{
    _connected             = true;
    _connecting            = false;

    std::string ip_address = GetRemoteIpAddress().to_string();
    TC_LOG_DEBUG("masterclient", "Connected to {}", ip_address);

    AsyncRead();

    // Reset the timer to immediately start receiving.
    _updateTimer.expires_from_now(boost::posix_time::milliseconds(100));
    _updateTimer.async_wait([this](boost::system::error_code const&) { UpdateSocket(); });
}

void MasterServerClient::OnClose()
{
    _connecting = false;
    _connected  = false;
}

void MasterServerClient::SendPacket(MasterServerPacket const& packet) {
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

bool MasterServerClient::Update()
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
                _headerBuffer.Reset();
                break;
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

    if (!MasterServerSocket::Update())
        return false;

    return true;
}

bool MasterServerClient::ReadHeaderHandler()
{
    ASSERT(_headerBuffer.GetActiveSize() == sizeof(MasterPktHeader));

    MasterPktHeader* header = reinterpret_cast<MasterPktHeader*>(_headerBuffer.GetReadPointer());
    EndianConvertReverse(header->size);
    EndianConvert(header->cmd);

    if (!(header->size >= 0 && header->size < 10240) || !header->IsValidOpcode())
    {
        TC_LOG_ERROR("network",
                     "MasterServerClient::ReadHeaderHandler(): client {} sent malformed packet (size: {}, cmd: {})",
                     GetRemoteIpAddress().to_string(), header->size, header->cmd);
        return false;
    }

    _packetBuffer.Resize(header->size);
    return true;
}

void MasterServerClient::ReadHandler() {
    AsyncRead();
}
