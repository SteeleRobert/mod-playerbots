/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LLMTELEMETRY_H
#define PLAYERBOTS_LLMTELEMETRY_H

/*
 * Feeds the bot dashboard from this module.
 *
 * The dashboard (~/bot-dashboard) reads two tables owned by the sibling Ollama
 * modules. Rather than switch those modules on - mod-ollama-bot-buddy clears the
 * non-combat engine every tick, which would delete the very machinery this layer
 * steers - we write the same telemetry ourselves:
 *
 *   mod_ollama_bot_buddy_journal  one row per decision. The dashboard also mines
 *                                 the `prompt` column for live positions, by
 *                                 locating the literal "Bot state summary:" and
 *                                 parsing the next ~600 bytes. LlmPrompt emits
 *                                 that header in exactly the expected format.
 *   mod_ollama_chat_bot_events    died / quest_done / leveled_up, with position,
 *                                 driving the deaths list, map markers, journey
 *                                 replay and the per-bot counters.
 *
 * We never CREATE these tables: they belong to the other modules. If a table is
 * absent, its writes are skipped silently and everything else carries on.
 */

#include "Define.h"
#include "ObjectGuid.h"

#include <string>

class Player;

namespace LlmTelemetry
{
    // Canonical event names, matching mod-ollama-chat's MEMORY_EVENT_* constants.
    // The dashboard queries 'died' and 'quest_done' by name and tallies the rest.
    constexpr char const* EVENT_DIED       = "died";
    constexpr char const* EVENT_QUEST_DONE = "quest_done";
    constexpr char const* EVENT_LEVELED_UP = "leveled_up";
    constexpr char const* EVENT_STUCK      = "stuck";

    // One decision -> one journal row. `command` is the directive verb, so the
    // dashboard's per-command breakdown shows quest/grind/travel/turnin/vendor.
    void WriteDecision(Player* bot, std::string const& command, std::string const& params,
                       std::string const& reasoning, bool succeeded, std::string const& outcome,
                       uint32 latencyMs, std::string const& prompt, std::string const& reply);

    // A notable thing that happened, stamped with where the bot was when it did.
    void RecordEvent(Player* bot, char const* eventType, std::string const& detailJson = "");
}

#endif
