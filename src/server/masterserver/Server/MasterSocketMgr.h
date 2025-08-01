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

#ifndef MasterSocketMgr_h__
#define MasterSocketMgr_h__

#include "MasterSession.h"
#include "SocketMgr.h"

class MasterSocketMgr : public SocketMgr<MasterSession>
{
    typedef SocketMgr<MasterSession> BaseSocketMgr;

  public:
    static MasterSocketMgr& Instance()
    {
        static MasterSocketMgr instance;
        return instance;
    }

    bool StartNetwork(Trinity::Asio::IoContext& ioContext, std::string const& bindIp, uint16 port,
                      int threadCount = 1) override
    {
        if (!BaseSocketMgr::StartNetwork(ioContext, bindIp, port, threadCount))
            return false;

        _acceptor->AsyncAcceptWithCallback<&MasterSocketMgr::OnSocketAccept>();
        return true;
    }

  protected:
    NetworkThread<MasterSession>* CreateThreads() const override
    {
        return new NetworkThread<MasterSession>[1];
    }

    static void OnSocketAccept(tcp::socket&& sock, uint32 threadIndex)
    {
        Instance().OnSocketOpen(std::forward<tcp::socket>(sock), threadIndex);
    }
};

#define sMasterSocketMgr MasterSocketMgr::Instance()

#endif // MasterSocketMgr_h__
