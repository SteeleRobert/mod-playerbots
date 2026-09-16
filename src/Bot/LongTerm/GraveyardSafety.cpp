/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GraveyardSafety.h"

#include "Corpse.h"
#include "DBCStores.h"
#include "GameGraveyard.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "TravelMgr.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    // Graveyard id -> per-team assessment. Spawn data never changes while the
    // server runs, so an entry is computed once and kept.
    std::unordered_map<uint64, GraveyardSafety::Assessment> g_graveyardCache;
    std::unordered_map<uint64, bool> g_corpseCache;
    std::mutex g_mutex;

    uint64 CacheKey(uint32 graveyardId, TeamId team)
    {
        return (uint64(graveyardId) << 8) | uint64(team);
    }

    // The faction template the reaction is measured against: 1 is the Alliance
    // player template, 2 the Horde one, as TravelNodePath::calculateCost uses.
    FactionTemplateEntry const* TeamFactionTemplate(TeamId team)
    {
        return sFactionTemplateStore.LookupEntry(team == TEAM_HORDE ? 2 : 1);
    }

    uint32 LevelDelta()
    {
        return sPlayerbotAIConfig.llmDirectiveLethalGraveyardLevelDelta;
    }

    float Radius()
    {
        return float(sPlayerbotAIConfig.llmDirectiveLethalGraveyardRadius);
    }

    bool ZoneFitsBot(uint32 zoneId, Player* bot)
    {
        uint32 low = 0, high = 0;
        if (sTravelMgr.GetZoneLevelBracket(zoneId, low, high))
            return bot->GetLevel() >= low && bot->GetLevel() <= high;

        // Capitals carry no bracket and are never dangerous.
        for (uint32 capital : sTravelMgr.GetCapitalZones(bot->GetTeamId()))
            if (capital == zoneId)
                return true;

        return false;
    }

    // Can this team's ghost use this graveyard? The core answers that through
    // graveyard_zone links, keyed by the ghost's area first and zone second.
    bool UsableByTeam(GraveyardStruct const* graveyard, uint32 zoneId, uint32 areaId, TeamId team)
    {
        if (GraveyardData const* link = sGraveyard->FindGraveyardData(graveyard->ID, areaId))
            if (link->IsNeutralOrFriendlyToTeam(team))
                return true;
        if (GraveyardData const* link = sGraveyard->FindGraveyardData(graveyard->ID, zoneId))
            return link->IsNeutralOrFriendlyToTeam(team);
        return false;
    }

    // Zone and area of a graveyard. Cheap (a terrain lookup) but asked for every
    // graveyard on a map at once, so cached too.
    std::unordered_map<uint32, std::pair<uint32, uint32>> g_zoneCache;

    void ZoneOf(GraveyardStruct const* graveyard, uint32& zoneId, uint32& areaId)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_zoneCache.find(graveyard->ID);
            if (it != g_zoneCache.end())
            {
                zoneId = it->second.first;
                areaId = it->second.second;
                return;
            }
        }
        sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, zoneId, areaId, graveyard->Map, graveyard->x, graveyard->y,
                                  graveyard->z);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_zoneCache[graveyard->ID] = {zoneId, areaId};
    }
}

namespace GraveyardSafety
{
    Assessment AssessPosition(WorldPosition const& pos, TeamId team)
    {
        Assessment result;
        sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, result.zoneId, result.areaId, pos.GetMapId(),
                                  pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ());

        FactionTemplateEntry const* us = TeamFactionTemplate(team);
        if (!us)
            return result;

        std::unordered_map<uint32, bool> hostileByFaction;
        WorldPosition scan(pos);
        for (CreatureData const* data : scan.getCreaturesNear(Radius()))
        {
            CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(data->id);
            if (!info)
                continue;

            // Things that never attack: critters, spirit healers and other
            // untargetable NPCs, and anything flagged as never engaging.
            if (info->type == CREATURE_TYPE_CRITTER || (info->npcflag & UNIT_NPC_FLAG_SPIRITHEALER) ||
                (info->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE)))
                continue;

            auto it = hostileByFaction.find(info->faction);
            if (it == hostileByFaction.end())
            {
                FactionTemplateEntry const* theirs = sFactionTemplateStore.LookupEntry(info->faction);
                bool const hostile = theirs && Unit::GetFactionReactionTo(theirs, us) < REP_NEUTRAL;
                it = hostileByFaction.emplace(info->faction, hostile).first;
            }
            if (!it->second)
                continue;

            ++result.hostileCount;
            if (result.maxHostileLevel < info->maxlevel)
                result.maxHostileLevel = info->maxlevel;
        }

        return result;
    }

    Assessment const& AssessGraveyard(GraveyardStruct const* graveyard, TeamId team)
    {
        static Assessment const empty;
        if (!graveyard)
            return empty;

        uint64 const key = CacheKey(graveyard->ID, team);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_graveyardCache.find(key);
            if (it != g_graveyardCache.end())
                return it->second;
        }

        Assessment const fresh =
            AssessPosition(WorldPosition(graveyard->Map, graveyard->x, graveyard->y, graveyard->z), team);

        std::lock_guard<std::mutex> lock(g_mutex);
        return g_graveyardCache.emplace(key, fresh).first->second;
    }

    bool IsLethal(Assessment const& assessment, uint32 botLevel)
    {
        return assessment.hostileCount && assessment.maxHostileLevel >= botLevel + LevelDelta();
    }

    bool IsLethalFor(GraveyardStruct const* graveyard, Player* bot)
    {
        if (!graveyard || !bot)
            return false;
        return IsLethal(AssessGraveyard(graveyard, bot->GetTeamId()), bot->GetLevel());
    }

    bool IsCorpseLethal(Corpse* corpse, Player* bot)
    {
        if (!corpse || !bot)
            return false;

        // Keyed by corpse guid and bot level: a corpse never moves, and the same
        // corpse is asked about every tick until it is reclaimed or expires.
        uint64 const key = corpse->GetGUID().GetRawValue() ^ (uint64(bot->GetLevel()) << 56);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_corpseCache.find(key);
            if (it != g_corpseCache.end())
                return it->second;
        }

        bool const lethal = IsLethal(AssessPosition(WorldPosition(corpse), bot->GetTeamId()), bot->GetLevel());

        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_corpseCache.size() > 4096)
            g_corpseCache.clear();
        g_corpseCache[key] = lethal;
        return lethal;
    }

    GraveyardStruct const* CurrentGraveyard(Player* bot)
    {
        if (!bot)
            return nullptr;
        return sGraveyard->GetClosestGraveyard(bot, bot->GetTeamId());
    }

    GraveyardStruct const* FindSafeGraveyard(Player* bot, WorldPosition const& from, bool leaveZone,
                                             std::string& why)
    {
        why.clear();
        if (!bot)
            return nullptr;

        TeamId const team = bot->GetTeamId();
        uint32 const level = bot->GetLevel();
        uint32 fromZone = 0, fromArea = 0;
        sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, fromZone, fromArea, from.GetMapId(), from.GetPositionX(),
                                  from.GetPositionY(), from.GetPositionZ());

        // Rank first, assess second. The spawn scan behind an assessment walks
        // every creature spawn in the world, so it is only paid for candidates in
        // ranking order until one passes - normally the first one or two - and
        // each result is cached for good.
        struct Candidate
        {
            GraveyardStruct const* graveyard;
            float score;
            bool sameZone;
        };
        std::vector<Candidate> candidates;

        for (auto const& [id, graveyard] : sGraveyard->GetGraveyardData())
        {
            if (graveyard.Map != from.GetMapId())
                continue;

            uint32 zoneId = 0, areaId = 0;
            ZoneOf(&graveyard, zoneId, areaId);

            bool const sameZone = zoneId == fromZone;
            if (sameZone && leaveZone)
                continue;

            bool const fits = ZoneFitsBot(zoneId, bot);
            if (!sameZone && !fits)
                continue;

            // A ghost in the source zone may use graveyards linked to that zone;
            // for anything further afield, the graveyard's own zone decides.
            bool usable = false;
            if (sameZone)
            {
                if (GraveyardData const* link = sGraveyard->FindGraveyardData(graveyard.ID, fromZone))
                    usable = link->IsNeutralOrFriendlyToTeam(team);
            }
            if (!usable)
                usable = UsableByTeam(&graveyard, zoneId, areaId, team);
            if (!usable)
                continue;

            float const dx = graveyard.x - from.GetPositionX();
            float const dy = graveyard.y - from.GetPositionY();
            float score = std::sqrt(dx * dx + dy * dy);
            if (!fits)
                score *= 2.0f;

            candidates.push_back({&graveyard, score, sameZone});
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](Candidate const& a, Candidate const& b) { return a.score < b.score; });

        for (Candidate const& candidate : candidates)
        {
            if (IsLethal(AssessGraveyard(candidate.graveyard, team), level))
                continue;
            why = candidate.sameZone ? "nearest safe graveyard in the same zone"
                                     : "nearest safe graveyard in a level-appropriate zone";
            return candidate.graveyard;
        }

        // Nothing usable on the map: the bind point is always somewhere the bot
        // once stood alive, which is the best guess left.
        uint32 homeZone = 0, homeArea = 0;
        sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, homeZone, homeArea, bot->m_homebindMapId, bot->m_homebindX,
                                  bot->m_homebindY, bot->m_homebindZ);
        GraveyardStruct const* home =
            sGraveyard->GetClosestGraveyard(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY,
                                            bot->m_homebindZ, team, homeArea, homeZone,
                                            bot->getClass() == CLASS_DEATH_KNIGHT);
        if (home && !IsLethalFor(home, bot))
        {
            why = "home-bind graveyard";
            return home;
        }

        return nullptr;
    }
}
