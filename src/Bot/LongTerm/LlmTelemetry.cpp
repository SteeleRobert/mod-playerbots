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
#include "Timer.h"

#include <mutex>
#include <vector>

namespace
{
    constexpr size_t FIELD_CAP = 60000;
    constexpr size_t POSITION_BATCH_SIZE = 50;
    constexpr uint32 POSITION_BATCH_DELAY_MS = 1000;
    constexpr uint32 POSITION_CLEANUP_INTERVAL_MS = 60 * 1000;
    constexpr uint32 POSITION_CLEANUP_LIMIT = 10000;

    struct PositionSample
    {
        uint64 botGuid;
        uint32 map;
        uint32 zone;
        uint32 area;
        float x;
        float y;
        float z;
        uint32 level;
    };

    // LlmTelemetry is called from the world thread. Keeping the small buffer here
    // avoids retaining Player pointers and turns N per-bot writes into one queued
    // multi-row statement.
    std::vector<PositionSample> pendingPositions;
    uint32 pendingPositionsSinceMs = 0;
    uint32 lastPositionCleanupMs = 0;

    std::string Escaped(std::string value)
    {
        if (value.size() > FIELD_CAP)
            value = value.substr(0, FIELD_CAP) + "...[truncated]";
        CharacterDatabase.EscapeString(value);
        return value;
    }

    // Some telemetry tables belong to sibling modules and the track table arrives
    // through this module's character-DB update. A missing optional table is not
    // worth a log line per sample: probe once, then stay quiet.
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
        if (!TableUsable("playerbots_llm_journal"))
            return;

        std::string const sql =
            "INSERT INTO playerbots_llm_journal "
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
        if (!TableUsable("playerbots_llm_events"))
            return;

        // `detail` is a JSON column: it takes valid JSON or NULL, never a bare string.
        std::string const detail = detailJson.empty() ? std::string("NULL")
                                                      : ("'" + Escaped(detailJson) + "'");

        std::string const sql =
            "INSERT INTO playerbots_llm_events "
            "(bot_guid, event_type, map, zone, area, x, y, z, bot_level, count, detail) VALUES (" +
            std::to_string(bot->GetGUID().GetRawValue()) + ",'" + Escaped(eventType) + "'," +
            std::to_string(bot->GetMapId()) + "," + std::to_string(bot->GetZoneId()) + "," +
            std::to_string(bot->GetAreaId()) + "," + std::to_string(bot->GetPositionX()) + "," +
            std::to_string(bot->GetPositionY()) + "," + std::to_string(bot->GetPositionZ()) + "," +
            std::to_string(uint32(bot->GetLevel())) + ",1," + detail + ")";

        CharacterDatabase.Execute(sql.c_str());
    }

    void SamplePosition(Player* bot, uint32 nowMs)
    {
        if (!bot || !sPlayerbotAIConfig.llmDirectiveDashboardTelemetry ||
            !sPlayerbotAIConfig.llmDirectivePositionSampleSeconds)
            return;
        if (!TableUsable("playerbots_llm_bot_track"))
            return;

        if (pendingPositions.empty())
            pendingPositionsSinceMs = nowMs;

        pendingPositions.push_back({bot->GetGUID().GetRawValue(), bot->GetMapId(), bot->GetZoneId(),
                                    bot->GetAreaId(), bot->GetPositionX(), bot->GetPositionY(),
                                    bot->GetPositionZ(), uint32(bot->GetLevel())});

        if (pendingPositions.size() >= POSITION_BATCH_SIZE)
            FlushPositionSamples(nowMs, true);
    }

    void FlushPositionSamples(uint32 nowMs, bool force)
    {
        if (pendingPositions.empty())
            return;
        if (!force && pendingPositions.size() < POSITION_BATCH_SIZE &&
            getMSTimeDiff(pendingPositionsSinceMs, nowMs) < POSITION_BATCH_DELAY_MS)
            return;

        std::string sql =
            "INSERT INTO playerbots_llm_bot_track "
            "(bot_guid, map, zone, area, x, y, z, bot_level) VALUES ";
        sql.reserve(sql.size() + pendingPositions.size() * 100);

        bool first = true;
        for (PositionSample const& sample : pendingPositions)
        {
            if (!first)
                sql += ',';
            first = false;
            sql += '(' + std::to_string(sample.botGuid) + ',' + std::to_string(sample.map) + ',' +
                   std::to_string(sample.zone) + ',' + std::to_string(sample.area) + ',' +
                   std::to_string(sample.x) + ',' + std::to_string(sample.y) + ',' +
                   std::to_string(sample.z) + ',' + std::to_string(sample.level) + ')';
        }

        CharacterDatabase.Execute(sql);
        pendingPositions.clear();
        pendingPositionsSinceMs = 0;

        if (!lastPositionCleanupMs ||
            getMSTimeDiff(lastPositionCleanupMs, nowMs) >= POSITION_CLEANUP_INTERVAL_MS)
        {
            std::string const cleanupSql =
                "DELETE FROM playerbots_llm_bot_track WHERE sampled_at < "
                "CURRENT_TIMESTAMP(3) - INTERVAL " + std::to_string(sPlayerbotAIConfig.llmDirectivePositionRetentionDays) +
                " DAY LIMIT " + std::to_string(POSITION_CLEANUP_LIMIT);
            CharacterDatabase.Execute(cleanupSql.c_str());
            lastPositionCleanupMs = nowMs;
        }
    }
}
