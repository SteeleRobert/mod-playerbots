/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestAnchor.h"

#include "Log.h"
#include "ObjectMgr.h"
#include "QuestDef.h"

#include <cmath>
#include <limits>

void QuestAnchor::EnsureBuilt()
{
    if (_built)
        return;
    _built = true;

    // One pass over every spawn in the world. Done once, lazily, so a server that
    // never enables the LLM layer never pays for it.
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        _creatureSpawns[data.id].push_back({data.mapid, data.posX, data.posY, data.posZ});

    // ObjectMgr only exposes creature -> quest; invert it so a quest can find who
    // takes it back.
    if (QuestRelations* involved = sObjectMgr->GetCreatureQuestInvolvedRelationMap())
        for (auto const& [creatureEntry, questId] : *involved)
            _questEnders[questId].push_back(creatureEntry);

    LOG_INFO("playerbots",
             "[QuestAnchor] indexed {} creature entries, {} quest enders",
             _creatureSpawns.size(), _questEnders.size());
}

bool QuestAnchor::Nearest(std::vector<Anchor> const& candidates, uint32 mapId, float x, float y,
                          Anchor& out) const
{
    float best = std::numeric_limits<float>::max();
    bool found = false;

    for (Anchor const& a : candidates)
    {
        if (a.mapId != mapId)
            continue;

        float const dx = a.x - x;
        float const dy = a.y - y;
        float const d2 = dx * dx + dy * dy;
        if (d2 < best)
        {
            best = d2;
            out = a;
            found = true;
        }
    }

    // The POI names a neighbourhood; a spawn on the far side of the map that
    // happens to share a creature entry is not what it meant.
    constexpr float MAX_ANCHOR_DIST = 300.0f;
    return found && best <= MAX_ANCHOR_DIST * MAX_ANCHOR_DIST;
}

bool QuestAnchor::FindObjectiveAnchor(Quest const* quest, uint32 mapId, float poiX, float poiY,
                                      Anchor& out)
{
    if (!quest)
        return false;

    EnsureBuilt();

    // A positive RequiredNpcOrGo is a creature entry; a negative one is a
    // gameobject entry. Take whichever spawn sits closest to the POI.
    Anchor best;
    float bestDist = std::numeric_limits<float>::max();
    bool found = false;

    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        int32 const entry = quest->RequiredNpcOrGo[i];
        if (!entry)
            continue;

        // A negative entry means a gameobject. ObjectMgr does not expose the
        // gameobject spawn table, so those objectives keep the old behaviour for
        // now - they are rarer than creature kills and worth a follow-up rather
        // than a worse guess here.
        if (entry < 0)
            continue;

        auto const itr = _creatureSpawns.find(static_cast<uint32>(entry));
        if (itr == _creatureSpawns.end())
            continue;

        Anchor candidate;
        if (!Nearest(itr->second, mapId, poiX, poiY, candidate))
            continue;

        float const dx = candidate.x - poiX;
        float const dy = candidate.y - poiY;
        float const d2 = dx * dx + dy * dy;
        if (d2 < bestDist)
        {
            bestDist = d2;
            best = candidate;
            found = true;
        }
    }

    if (found)
        out = best;
    return found;
}

bool QuestAnchor::FindTurnInAnchor(uint32 questId, uint32 mapId, float poiX, float poiY, Anchor& out)
{
    EnsureBuilt();

    auto const itr = _questEnders.find(questId);
    if (itr == _questEnders.end())
        return false;

    Anchor best;
    float bestDist = std::numeric_limits<float>::max();
    bool found = false;

    for (uint32 entry : itr->second)
    {
        auto const spawns = _creatureSpawns.find(entry);
        if (spawns == _creatureSpawns.end())
            continue;

        Anchor candidate;
        if (!Nearest(spawns->second, mapId, poiX, poiY, candidate))
            continue;

        float const dx = candidate.x - poiX;
        float const dy = candidate.y - poiY;
        float const d2 = dx * dx + dy * dy;
        if (d2 < bestDist)
        {
            bestDist = d2;
            best = candidate;
            found = true;
        }
    }

    if (found)
        out = best;
    return found;
}
