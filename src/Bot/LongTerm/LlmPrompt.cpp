/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LlmPrompt.h"

#include "LlmJournal.h"
#include "DBCStores.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

#include <sstream>

namespace
{
    char const* ClassName(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return "Warrior";
            case CLASS_PALADIN:      return "Paladin";
            case CLASS_HUNTER:       return "Hunter";
            case CLASS_ROGUE:        return "Rogue";
            case CLASS_PRIEST:       return "Priest";
            case CLASS_DEATH_KNIGHT: return "Death Knight";
            case CLASS_SHAMAN:       return "Shaman";
            case CLASS_MAGE:         return "Mage";
            case CLASS_WARLOCK:      return "Warlock";
            case CLASS_DRUID:        return "Druid";
            default:                 return "Adventurer";
        }
    }

    char const* RpgStatusName(NewRpgStatus status)
    {
        switch (status)
        {
            case RPG_IDLE:          return "idle";
            case RPG_GO_GRIND:      return "walking to a grind spot";
            case RPG_GO_CAMP:       return "walking to a town";
            case RPG_WANDER_RANDOM: return "roaming and killing nearby";
            case RPG_WANDER_NPC:    return "visiting nearby NPCs";
            case RPG_DO_QUEST:      return "working a quest";
            case RPG_TRAVEL_FLIGHT: return "taking a flight";
            case RPG_REST:          return "resting";
            default:                return "unknown";
        }
    }

    std::string Money(uint32 copper)
    {
        std::ostringstream out;
        out << (copper / 10000) << "g " << ((copper % 10000) / 100) << "s";
        return out.str();
    }

    // Keeps free text out of the JSON body's way and out of the fmt formatter's way.
    std::string Sanitize(std::string const& in, size_t cap)
    {
        std::string out;
        out.reserve(std::min(in.size(), cap));
        for (char c : in)
        {
            if (out.size() >= cap)
                break;
            if (c == '\n' || c == '\r' || c == '\t')
                out.push_back(' ');
            else if (c == '"' || c == '\\' || c == '{' || c == '}')
                out.push_back(' ');
            else
                out.push_back(c);
        }
        return out;
    }

    void AppendQuestLog(Player* bot, std::ostringstream& out, uint32& completeCount)
    {
        completeCount = 0;
        uint32 listed = 0;

        for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            uint32 const questId = bot->GetQuestSlotQuestId(slot);
            if (!questId)
                continue;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                continue;

            QuestStatusMap const& statusMap = bot->getQuestStatusMap();
            auto statusItr = statusMap.find(questId);
            if (statusItr == statusMap.end())
                continue;
            QuestStatusData const& status = statusItr->second;

            bool const isComplete = status.Status == QUEST_STATUS_COMPLETE;
            if (isComplete)
                ++completeCount;

            // The log can hold 25; listing every one of them crowds out the parts of
            // the prompt that actually change the decision.
            if (listed >= 12)
                continue;
            ++listed;

            out << "- " << (isComplete ? "[READY TO TURN IN] " : "") << Sanitize(quest->GetTitle(), 60)
                << " (lvl " << quest->GetQuestLevel() << ")";

            int32 const questZone = quest->GetZoneOrSort();
            if (questZone > 0)
            {
                std::string const zoneName = LlmZones::NameOf(static_cast<uint32>(questZone));
                if (!zoneName.empty())
                    out << " in " << zoneName;
            }

            if (!isComplete)
            {
                bool first = true;
                for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                {
                    if (!quest->RequiredNpcOrGo[i] || !quest->RequiredNpcOrGoCount[i])
                        continue;
                    out << (first ? ": " : ", ") << status.CreatureOrGOCount[i] << "/"
                        << quest->RequiredNpcOrGoCount[i] << " killed/used";
                    first = false;
                }
                for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                {
                    if (!quest->RequiredItemId[i] || !quest->RequiredItemCount[i])
                        continue;
                    out << (first ? ": " : ", ") << status.ItemCount[i] << "/"
                        << quest->RequiredItemCount[i] << " collected";
                    first = false;
                }
                if (first)
                    out << ": no countable objectives";
            }
            out << "\n";
        }

        if (!listed)
            out << "- (empty)\n";
    }

    uint32 CountNearbyQuestGivers(Player* bot, PlayerbotAI* botAI)
    {
        uint32 count = 0;
        GuidVector const targets =
            botAI->GetAiObjectContext()->GetValue<GuidVector>("possible new rpg targets")->Get();
        for (ObjectGuid const& guid : targets)
        {
            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->ToCreature())
                continue;
            if (unit->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                ++count;
        }
        return count;
    }
}

namespace LlmPrompt
{
    std::string BuildDecisionPrompt(Player* bot, uint32 deathsSinceLastDecision,
                                    std::vector<LlmZoneChoice>& legalZones)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return "";

        legalZones = LlmZones::LegalZonesFor(bot);

        uint32 const zoneId = bot->GetZoneId();
        std::string zoneName = LlmZones::NameOf(zoneId);
        if (zoneName.empty())
            zoneName = "Unknown";

        uint32 const maxHealth = std::max<uint32>(1, bot->GetMaxHealth());
        uint32 const healthPct = static_cast<uint32>(bot->GetHealth() * 100 / maxHealth);

        // Reuse the values the classical engine already computes for these, rather
        // than growing a second, subtly different notion of "how full are my bags".
        uint8 const bagSpaceUsedPct = botAI->GetAiObjectContext()->GetValue<uint8>("bag space")->Get();
        uint8 const durabilityPct = botAI->GetAiObjectContext()->GetValue<uint8>("durability")->Get();

        std::ostringstream out;

        // The bot dashboard mines this header out of the journalled prompt to get
        // live positions: it locates the literal "Bot state summary:" and parses
        // the next ~600 bytes with ^Name:/^Level:/^Area:/^Zone:/^Map:/^Position:.
        // Keep the field names, the order and the leading line exactly as they are
        // - see botdash/calls.py. It is also genuinely useful context for the model,
        // which is why it goes in the real prompt rather than only in the copy.
        {
            char const* areaName = "Unknown";
            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetAreaId()))
                if (area->area_name[LOCALE_enUS])
                    areaName = area->area_name[LOCALE_enUS];

            char const* mapName = "Unknown";
            if (MapEntry const* mapEntry = sMapStore.LookupEntry(bot->GetMapId()))
                if (mapEntry->name[LOCALE_enUS])
                    mapName = mapEntry->name[LOCALE_enUS];

            out << "Bot state summary:\n"
                << "Name: " << bot->GetName() << "\n"
                << "Level: " << uint32(bot->GetLevel()) << "\n"
                << "Class: " << ClassName(bot->getClass()) << "\n"
                << "Gold: " << (bot->GetMoney() / 10000) << "\n"
                << "Area: " << areaName << "\n"
                << "Zone: " << zoneName << "\n"
                << "Map: " << mapName << "\n"
                << "Position: " << bot->GetPositionX() << " " << bot->GetPositionY() << " "
                << bot->GetPositionZ() << "\n\n";
        }

        out << "You are the strategic planner for " << bot->GetName() << ", a level "
            << uint32(bot->GetLevel()) << " " << ClassName(bot->getClass())
            << " in World of Warcraft.\n"
            << "You decide ONLY the broad intent for the next few minutes. Moving, fighting, "
               "looting, pathing and talking to NPCs are handled by another system - never plan those.\n\n";

        out << "CURRENT STATE\n"
            << "- Zone: " << zoneName << "\n"
            << "- Health: " << healthPct << "%\n"
            << "- Money: " << Money(bot->GetMoney()) << "\n"
            << "- Bag space used: " << uint32(bagSpaceUsedPct) << "%\n"
            << "- Equipment durability: " << uint32(durabilityPct) << "%\n"
            << "- Right now the bot is: " << RpgStatusName(botAI->rpgInfo.GetStatus()) << "\n"
            << "- Deaths since your last decision: " << deathsSinceLastDecision << "\n"
            << "- Quests so far: accepted " << botAI->rpgStatistic.questAccepted
            << ", turned in " << botAI->rpgStatistic.questRewarded
            << ", given up on " << botAI->rpgStatistic.questAbandoned << "\n"
            << "- Quest givers within sight: " << CountNearbyQuestGivers(bot, botAI) << "\n\n";

        uint32 completeCount = 0;
        std::ostringstream questLog;
        AppendQuestLog(bot, questLog, completeCount);
        out << "QUEST LOG (" << completeCount << " ready to turn in)\n" << questLog.str() << "\n";

        std::vector<LlmHistoryEntry> const history =
            LlmJournal::RecentHistory(bot->GetGUID(), sPlayerbotAIConfig.llmDirectiveHistorySize);
        out << "YOUR RECENT DECISIONS (oldest first)\n";
        if (history.empty())
        {
            out << "- none yet\n";
        }
        else
        {
            for (LlmHistoryEntry const& entry : history)
            {
                out << "- " << entry.directive;
                if (!entry.reason.empty())
                    out << " (\"" << Sanitize(entry.reason, 120) << "\")";
                out << " -> " << (entry.outcome.empty() ? "still running" : Sanitize(entry.outcome, 160)) << "\n";
            }
        }
        out << "\n";

        out << "LEGAL ACTIONS - pick exactly one:\n"
            << "  quest  - keep working the quest log where you are\n"
            << "  grind  - farm mobs for experience instead of questing\n"
            << "  travel - fly to a different zone (you MUST name a zone)\n"
            << "  turnin - go hand in the quests that are already complete\n"
            << "  vendor - head to a town to repair, sell and restock\n\n";

        out << "LEGAL ZONES - copy one of these names exactly, or use \"\" to stay put:\n";
        for (size_t i = 0; i < legalZones.size(); ++i)
            out << (i ? " | " : "  ") << legalZones[i].name;
        out << "\n\n";

        out << "Answer with ONE JSON object and nothing else, in exactly this shape:\n"
            << "{\"action\":\"quest|grind|travel|turnin|vendor\",\"zone\":\"\",\"reason\":\"under 20 words\"}\n";

        return out.str();
    }
}
