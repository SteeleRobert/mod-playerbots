/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LlmJournal.h"

#include "DatabaseEnv.h"
#include "Log.h"
#include "PlayerbotAIConfig.h"

#include <deque>
#include <mutex>
#include <unordered_map>

namespace
{
    std::mutex g_mutex;
    std::unordered_map<uint64, std::deque<LlmHistoryEntry>> g_history;
    constexpr size_t HISTORY_CAP = 12;

    // MEDIUMTEXT would hold far more, but a runaway prompt or a model that never
    // stops talking should not be able to turn one bad tick into a multi-megabyte
    // insert on every decision.
    constexpr size_t FIELD_CAP = 60000;

    std::string Escaped(std::string value)
    {
        if (value.size() > FIELD_CAP)
            value = value.substr(0, FIELD_CAP) + "...[truncated]";
        PlayerbotsDatabase.EscapeString(value);
        return value;
    }
}

namespace LlmJournal
{
    void EnsureSchema()
    {
        PlayerbotsDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `playerbots_llm_directive_journal` ("
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
            "`ts` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
            "`bot_guid` BIGINT UNSIGNED NOT NULL,"
            "`bot_name` VARCHAR(24) NOT NULL DEFAULT '',"
            "`bot_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`bot_class` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "`zone_id` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`model` VARCHAR(128) NOT NULL DEFAULT '',"
            "`endpoint` VARCHAR(255) NOT NULL DEFAULT '',"
            "`num_predict` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`latency_ms` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`parse_ok` TINYINT(1) NOT NULL DEFAULT 0,"
            "`parse_error` VARCHAR(255) NOT NULL DEFAULT '',"
            "`reply_chars` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`action` VARCHAR(16) NOT NULL DEFAULT '',"
            "`chosen_zone_id` INT UNSIGNED NOT NULL DEFAULT 0,"
            "`chosen_zone` VARCHAR(100) NOT NULL DEFAULT '',"
            "`reason` VARCHAR(512) NOT NULL DEFAULT '',"
            "`prev_directive` VARCHAR(128) NOT NULL DEFAULT '',"
            "`prev_outcome` VARCHAR(512) NOT NULL DEFAULT '',"
            "`prompt` MEDIUMTEXT,"
            "`reply` MEDIUMTEXT,"
            "PRIMARY KEY (`id`),"
            "KEY `bot_ts` (`bot_guid`, `ts`),"
            "KEY `parse_ok_ts` (`parse_ok`, `ts`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
    }

    void Write(LlmJournalRecord const& r)
    {
        // Created on first use rather than at startup: a server that never turns the
        // feature on never grows the table, and a server whose SQL updater is off
        // still gets a working journal.
        if (sPlayerbotAIConfig.llmDirectiveJournalAutoCreate)
        {
            static std::once_flag schemaOnce;
            std::call_once(schemaOnce, []() { EnsureSchema(); });
        }

        std::string const sql =
            "INSERT INTO playerbots_llm_directive_journal "
            "(bot_guid, bot_name, bot_level, bot_class, zone_id, model, endpoint, num_predict, latency_ms, "
            " parse_ok, parse_error, reply_chars, action, chosen_zone_id, chosen_zone, reason, "
            " prev_directive, prev_outcome, prompt, reply) VALUES (" +
            std::to_string(r.botGuid.GetRawValue()) + ",'" + Escaped(r.botName) + "'," +
            std::to_string(r.botLevel) + "," + std::to_string(r.botClass) + "," +
            std::to_string(r.zoneId) + ",'" + Escaped(r.model) + "','" + Escaped(r.endpoint) + "'," +
            std::to_string(r.numPredict) + "," + std::to_string(r.latencyMs) + "," +
            (r.parseOk ? "1" : "0") + ",'" + Escaped(r.parseError) + "'," +
            std::to_string(r.replyChars) + ",'" + Escaped(r.action) + "'," +
            std::to_string(r.chosenZoneId) + ",'" + Escaped(r.chosenZone) + "','" +
            Escaped(r.reason) + "','" + Escaped(r.prevDirective) + "','" + Escaped(r.prevOutcome) + "','" +
            Escaped(r.prompt) + "','" + Escaped(r.reply) + "')";

        PlayerbotsDatabase.Execute(sql.c_str());
    }

    void PushHistory(ObjectGuid guid, LlmHistoryEntry entry)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto& dq = g_history[guid.GetRawValue()];
        dq.push_back(std::move(entry));
        while (dq.size() > HISTORY_CAP)
            dq.pop_front();
    }

    void SetLastOutcome(ObjectGuid guid, std::string const& outcome)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_history.find(guid.GetRawValue());
        if (it == g_history.end() || it->second.empty())
            return;
        it->second.back().outcome = outcome;
    }

    std::vector<LlmHistoryEntry> RecentHistory(ObjectGuid guid, uint32 count)
    {
        std::vector<LlmHistoryEntry> out;
        if (!count)
            return out;

        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_history.find(guid.GetRawValue());
        if (it == g_history.end())
            return out;

        auto const& dq = it->second;
        size_t const start = dq.size() > count ? dq.size() - count : 0;
        for (size_t i = start; i < dq.size(); ++i)
            out.push_back(dq[i]);
        return out;
    }

    void Forget(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_history.erase(guid.GetRawValue());
    }
}
