/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GRAVEYARDSAFETY_H
#define PLAYERBOTS_GRAVEYARDSAFETY_H

/*
 * Is this graveyard survivable for a bot of level L?
 *
 * The core picks a ghost's graveyard purely by zone link and distance, and a
 * random bot that cannot teleport (honest mode) stands up wherever that is. At
 * Ravenhill in Duskwood that is 32 yards from a pack of level 24 undead, and a
 * level 11 bot resurrected there dies again within the minute, forever.
 *
 * The answer is computed from spawn data rather than live objects, exactly as
 * TravelNodePath::calculateCost does, so it works when the grid is not loaded.
 * It is static for the lifetime of the server, so it is cached per graveyard.
 */

#include "Define.h"
#include "SharedDefines.h"

#include <string>

class Corpse;
class Player;
class WorldPosition;
struct GraveyardStruct;

namespace GraveyardSafety
{
    struct Assessment
    {
        uint32 maxHostileLevel{0};  // highest maxlevel of any hostile spawn in range
        uint32 hostileCount{0};     // how many hostile spawn points are in range
        uint32 zoneId{0};           // zone the assessed point lies in
        uint32 areaId{0};
    };

    // Spawn-data assessment of a point, for the given faction. Uncached.
    Assessment AssessPosition(WorldPosition const& pos, TeamId team);

    // Cached per graveyard id and team.
    Assessment const& AssessGraveyard(GraveyardStruct const* graveyard, TeamId team);

    // A point is lethal when something hostile there outlevels the bot by the
    // configured margin (AiPlayerbot.LlmDirective.LethalGraveyardLevelDelta).
    bool IsLethal(Assessment const& assessment, uint32 botLevel);
    bool IsLethalFor(GraveyardStruct const* graveyard, Player* bot);

    // Where the corpse lies, cached by corpse guid: FindCorpseAction asks every tick.
    bool IsCorpseLethal(Corpse* corpse, Player* bot);

    // The graveyard the core would repop this player at right now.
    GraveyardStruct const* CurrentGraveyard(Player* bot);

    // Nearest graveyard on the bot's map that the bot's faction may use and that
    // is not lethal for it. Candidates are the graveyards linked to the ghost's
    // own zone (unless `leaveZone`), plus any graveyard in a zone whose level
    // bracket fits the bot or that is a capital. Ranking is by distance, with a
    // graveyard in a zone the bot has outgrown or not grown into counted at
    // double distance, so a level-appropriate zone next door beats the far end
    // of a hostile one. Falls back to the bot's home-bind graveyard, then null.
    GraveyardStruct const* FindSafeGraveyard(Player* bot, WorldPosition const& from, bool leaveZone,
                                             std::string& why);
}

#endif
