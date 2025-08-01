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

#include "QueueMgr.h"
#include <Realm.h>
#include "QueueMasterServerHandler.h"

QueueMgr* QueueMgr::instance()
{
    static QueueMgr instance;
    return &instance;
}

uint32 QueueMgr::GetCurrentPlayerCount()
{
    return m_currentPlayerCount;
}

uint32 QueueMgr::GetPlayerLimit()
{
    return m_playerLimit;
}

void QueueMgr::AddSession(QueueSession* session) {
    m_QueuedPlayer.push_back(session);
}

bool QueueMgr::RemoveSession(QueueSession* session)
{
    uint32 position      = 1;
    Queue::iterator iter = m_QueuedPlayer.begin();

    // search to remove and count skipped positions
    bool found = false;

    for (; iter != m_QueuedPlayer.end(); ++iter, ++position)
    {
        if (*iter == session)
        {
            iter  = m_QueuedPlayer.erase(iter);
            found = true; // removing queued session
            break;
        }
    }

    // User not found, no need to update the queue
    if (!found)
        return false;

    // accept first in queue
    if ((!m_playerLimit || m_currentPlayerCount < m_playerLimit) && !m_QueuedPlayer.empty())
    {
        QueueSession* pop_sess = m_QueuedPlayer.front();
        pop_sess->Redirect();
        m_QueuedPlayer.pop_front();

        // update iter to point first queued socket or end() if queue is empty now
        iter     = m_QueuedPlayer.begin();
        position = 1;
    }

    // update position from iter to end()
    // iter point to first not updated socket, position store new position
    for (; iter != m_QueuedPlayer.end(); ++iter, ++position)
        (*iter)->SendAuthWaitQueue(position);

    return found;
}

void QueueMgr::Update(uint32 diff)
{
    _queueTimer += diff;

    if (_queueTimer >= 1000) // TODO dont hardcode
    {
        _queueTimer -= 1000;

        MasterServerPacket packet = MasterServerPacket(MASTER_MSG_REQUEST_OPEN_SLOTS, 0);
        sMasterServer->SendPacket(packet);
    }
}

void QueueMgr::HandleOpenSlotsResponse(uint32 slots)
{
    if (m_QueuedPlayer.empty())
        return;

    Queue::iterator iter = m_QueuedPlayer.begin();

    for (; iter != m_QueuedPlayer.end() && slots; --slots)
    {
        Queue::iterator current = iter++;
        (*current)->Redirect();        // Safe: still valid before erase
        m_QueuedPlayer.erase(current); // Safe: we already advanced iter
    }

    if (!m_QueuedPlayer.empty())
    {
        iter     = m_QueuedPlayer.begin();
        uint32 position = 1;

        // update position from iter to end()
        // iter point to first not updated socket, position store new position
        for (; iter != m_QueuedPlayer.end(); ++iter, ++position)
            (*iter)->SendAuthWaitQueue(position);
    }
}

Realm realm;
