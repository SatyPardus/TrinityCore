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

#include <Realm.h>
#include "MasterMgr.h"

MasterMgr* MasterMgr::instance()
{
    static MasterMgr instance;
    return &instance;
}

void MasterMgr::AddSession(MasterSession* session)
{
    m_sessions.push_back(session);
}

bool MasterMgr::RemoveSession(MasterSession* session)
{
    uint32 position      = 1;
    Sessions::iterator iter = m_sessions.begin();

    // search to remove and count skipped positions
    bool found = false;

    for (; iter != m_sessions.end(); ++iter, ++position)
    {
        if (*iter == session)
        {
            iter  = m_sessions.erase(iter);
            found = true;
            break;
        }
    }

    // User not found, no need to update the queue
    if (!found)
        return false;

    return found;
}
