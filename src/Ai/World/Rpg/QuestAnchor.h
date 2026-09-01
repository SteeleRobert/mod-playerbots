/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_QUESTANCHOR_H
#define PLAYERBOTS_QUESTANCHOR_H

/*
 * Real 3-D positions for quest objectives and turn-ins.
 *
 * The server's `quest_poi` data is TWO-DIMENSIONAL - X and Y only, no Z (see
 * quest_poi_points). The New RPG code therefore guessed the height by dropping a
 * ray from the sky:
 *
 *     dz = max(GetHeight(x, y, MAX_HEIGHT), GetWaterLevel(x, y))
 *
 * which lands on whatever is on TOP: the roof of an inn, or the hillside above a
 * mine. Measured on a live server: bots sent to "Skirmish at Echo Ridge" stood at
 * z 138 on the hill while the kobolds they had to kill were at z 84-92 inside the
 * mine, 50 yards below. They never made progress, and the quest was never
 * completed by any bot.
 *
 * Note that snapping to the "nearest walkable point" would NOT fix this: the
 * hilltop above a cave is perfectly walkable navmesh, so that query returns the
 * same wrong answer.
 *
 * The fix is to stop deriving height from geometry at all. Every objective type
 * this engine attempts refers to something that already has an exact position:
 *
 *     kill / interact  ->  the creature or gameobject spawn rows
 *     turn in          ->  the quest ender's spawn row
 *
 * The 2-D POI is still useful - it says WHICH cluster of spawns is meant, since a
 * creature type may live in several places - but it is never used for height.
 */

#include "Define.h"

#include <unordered_map>
#include <vector>

class Quest;

class QuestAnchor
{
public:
    static QuestAnchor& instance()
    {
        static QuestAnchor instance;
        return instance;
    }

    struct Anchor
    {
        uint32 mapId{0};
        float x{0.f}, y{0.f}, z{0.f};
    };

    // Built once, lazily, on first use: creature/gameobject entry -> spawn points,
    // and quest -> the creature entries that hand it in. ObjectMgr exposes the
    // forward relation only, so the reverse is derived here.
    void EnsureBuilt();

    // The spawn of a required creature/gameobject for `quest`, nearest to the 2-D
    // POI point. False when the quest has no such objective or nothing is spawned.
    bool FindObjectiveAnchor(Quest const* quest, uint32 mapId, float poiX, float poiY, Anchor& out);

    // The spawn of whoever this quest is handed in to, nearest to the POI point.
    bool FindTurnInAnchor(uint32 questId, uint32 mapId, float poiX, float poiY, Anchor& out);

    // The areatrigger that completes an exploration quest. Exact, and the only
    // anchor those quests have - they name no creature and carry no item.
    bool FindExplorationAnchor(uint32 questId, uint32 mapId, Anchor& out);

private:
    QuestAnchor() = default;

    bool _built{false};
    std::unordered_map<uint32, std::vector<Anchor>> _creatureSpawns;   // entry -> spawns
    std::unordered_map<uint32, std::vector<Anchor>> _gameObjectSpawns; // entry -> spawns
    // Exploration objectives: the quest's areatrigger, which carries a real
    // x/y/z and a radius. Loaded from the world DB - ObjectMgr exposes only
    // trigger -> quest, and the reverse is what is needed here.
    std::unordered_map<uint32, Anchor> _questAreaTriggers;
    std::unordered_map<uint32, std::vector<uint32>> _questEnders;      // quest -> creature entries

    bool Nearest(std::vector<Anchor> const& candidates, uint32 mapId, float x, float y, Anchor& out) const;
};

#define sQuestAnchor QuestAnchor::instance()

#endif
