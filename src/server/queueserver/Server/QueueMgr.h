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

#ifndef __QUEUEMGR_H
#define __QUEUEMGR_H

#include <Realm.h>

class QueueMgr
{
public:
    static QueueMgr* instance();

    void Initialize(std::string serverIp, uint16 serverPort);
    void AddSession(QueueSession* session);
    bool RemoveSession(QueueSession* session);
    void Update(uint32 diff);
    void HandleOpenSlotsResponse(uint32 slots);

    uint32 GetCurrentPlayerCount();
    uint32 GetPlayerLimit();

private:
    typedef std::list<QueueSession*> Queue;
    Queue m_QueuedPlayer;
    uint32 m_currentPlayerCount;
    uint32 m_playerLimit;

    std::string _serverIp;
    uint16 _serverPort;

    uint32 _queueTimer;
};

#define sQueue QueueMgr::instance()
extern Realm realm;

#endif
