/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "QuestAnchor.h"

#include "Log.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "QueryResult.h"
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

    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        _gameObjectSpawns[data.id].push_back({data.mapid, data.posX, data.posY, data.posZ});

    // Exploration quests name nothing that has a spawn, but their completion
    // areatrigger carries an exact x/y/z. ObjectMgr only maps trigger -> quest,
    // so read the relation straight from the world DB and invert it.
    if (QueryResult result = WorldDatabase.Query(
            "SELECT r.quest, a.map, a.x, a.y, a.z FROM areatrigger_involvedrelation r "
            "JOIN areatrigger a ON a.entry = r.id"))
    {
        do
        {
            Field* f = result->Fetch();
            _questAreaTriggers[f[0].Get<uint32>()] =
                {f[1].Get<uint32>(), f[2].Get<float>(), f[3].Get<float>(), f[4].Get<float>()};
        } while (result->NextRow());
    }

    // ObjectMgr only exposes creature -> quest; invert it so a quest can find who
    // takes it back.
    if (QuestRelations* involved = sObjectMgr->GetCreatureQuestInvolvedRelationMap())
        for (auto const& [creatureEntry, questId] : *involved)
            _questEnders[questId].push_back(creatureEntry);

    LOG_INFO("playerbots",
             "[QuestAnchor] indexed {} creature entries, {} gameobject entries, "
             "{} quest enders, {} exploration triggers",
             _creatureSpawns.size(), _gameObjectSpawns.size(), _questEnders.size(),
             _questAreaTriggers.size());
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

        // A positive entry is a creature, a negative one a gameobject.
        auto const& table = entry > 0 ? _creatureSpawns : _gameObjectSpawns;
        auto const itr = table.find(static_cast<uint32>(std::abs(entry)));
        if (itr == table.end())
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

    // Nothing to kill or click: if this is an exploration quest, its areatrigger
    // is the objective, and it is exact.
    if (!found)
        return FindExplorationAnchor(quest->GetQuestId(), mapId, out);

    out = best;
    return true;
}

bool QuestAnchor::FindExplorationAnchor(uint32 questId, uint32 mapId, Anchor& out)
{
    EnsureBuilt();

    auto const itr = _questAreaTriggers.find(questId);
    if (itr == _questAreaTriggers.end() || itr->second.mapId != mapId)
        return false;

    out = itr->second;
    return true;
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
