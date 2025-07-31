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

MasterServerClient::MasterServerClient(Trinity::Asio::IoContext& ioContext)
: MasterServerSocket(ioContext), _stopped(false), _updateThread(nullptr), _ioContext(1), _updateTimer(_ioContext),
  _connected(false), _connecting(true)
{
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

    _updateTimer.expires_from_now(boost::posix_time::milliseconds(1));
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
    TC_LOG_ERROR("session", "Connected to {}", ip_address);

    AsyncRead();
}

void MasterServerClient::OnClose()
{
    _connecting = false;
    _connected  = false;
}

bool MasterServerClient::Update()
{
    if (!MasterServerSocket::Update())
        return false;

    return true;
}

void MasterServerClient::ReadHandler() {
    AsyncRead();
}
