/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LlmTelemetry.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"

#include <mutex>

namespace
{
    constexpr size_t FIELD_CAP = 60000;

    std::string Escaped(std::string value)
    {
        if (value.size() > FIELD_CAP)
            value = value.substr(0, FIELD_CAP) + "...[truncated]";
        CharacterDatabase.EscapeString(value);
        return value;
    }

    // These tables belong to the sibling Ollama modules. On a server built without
    // them the tables simply are not there, and that is not an error worth a log
    // line per decision - probe once, then stay quiet.
    bool TableUsable(char const* table)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, bool> known;

        std::lock_guard<std::mutex> lock(mutex);
        auto it = known.find(table);
        if (it != known.end())
            return it->second;

        QueryResult result = CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = '" +
            std::string(table) + "' LIMIT 1");

        bool const usable = result != nullptr;
        known[table] = usable;
        if (!usable)
            LOG_INFO("playerbots", "[LlmTelemetry] table {} not present; dashboard feed for it is off", table);
        return usable;
    }
}

namespace LlmTelemetry
{
    void WriteDecision(Player* bot, std::string const& command, std::string const& params,
                       std::string const& reasoning, bool succeeded, std::string const& outcome,
                       uint32 latencyMs, std::string const& prompt, std::string const& reply)
    {
        if (!bot || !sPlayerbotAIConfig.llmDirectiveDashboardTelemetry)
            return;
        if (!TableUsable("mod_ollama_bot_buddy_journal"))
            return;

        std::string const sql =
            "INSERT INTO mod_ollama_bot_buddy_journal "
            "(bot_guid, bot_name, command, params, reasoning, succeeded, outcome, latency_ms, prompt, reply) VALUES (" +
            std::to_string(bot->GetGUID().GetRawValue()) + ",'" + Escaped(bot->GetName()) + "','" +
            Escaped(command) + "','" + Escaped(params) + "','" + Escaped(reasoning) + "'," +
            (succeeded ? "1" : "0") + ",'" + Escaped(outcome) + "'," + std::to_string(latencyMs) + ",'" +
            Escaped(prompt) + "','" + Escaped(reply) + "')";

        CharacterDatabase.Execute(sql.c_str());
    }

    void RecordEvent(Player* bot, char const* eventType, std::string const& detailJson)
    {
        if (!bot || !eventType || !sPlayerbotAIConfig.llmDirectiveDashboardTelemetry)
            return;
        if (!TableUsable("mod_ollama_chat_bot_events"))
            return;

        // `detail` is a JSON column: it takes valid JSON or NULL, never a bare string.
        std::string const detail = detailJson.empty() ? std::string("NULL")
                                                      : ("'" + Escaped(detailJson) + "'");

        std::string const sql =
            "INSERT INTO mod_ollama_chat_bot_events "
            "(bot_guid, event_type, map, zone, area, x, y, z, bot_level, count, detail) VALUES (" +
            std::to_string(bot->GetGUID().GetRawValue()) + ",'" + Escaped(eventType) + "'," +
            std::to_string(bot->GetMapId()) + "," + std::to_string(bot->GetZoneId()) + "," +
            std::to_string(bot->GetAreaId()) + "," + std::to_string(bot->GetPositionX()) + "," +
            std::to_string(bot->GetPositionY()) + "," + std::to_string(bot->GetPositionZ()) + "," +
            std::to_string(uint32(bot->GetLevel())) + ",1," + detail + ")";

        CharacterDatabase.Execute(sql.c_str());
    }
}
