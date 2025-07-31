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

#include "ClientSocket.h"
#include "IoContext.h"
#include "DeadlineTimer.h"

class MasterServerClient : public ClientSocket<MasterServerClient>
{
    typedef ClientSocket<MasterServerClient> MasterServerSocket;

public:
    MasterServerClient(Trinity::Asio::IoContext& ioContext);
    ~MasterServerClient();

    void Start() override;
    bool Update() override;

protected:
    void Run();
    void UpdateSocket();
    void OnClose() override;
    void ReadHandler() override;

    Trinity::Asio::IoContext _ioContext;
    std::thread* _updateThread;
    Trinity::Asio::DeadlineTimer _updateTimer;
    std::atomic<bool> _stopped;
    std::atomic<bool> _connected;
    std::atomic<bool> _connecting;
};
