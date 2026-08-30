/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LLMJOURNAL_H
#define PLAYERBOTS_LLMJOURNAL_H

/*
 * Every LLM call this module makes is written down: the exact prompt, the raw
 * reply, whether it parsed, what directive came out, how long it took and which
 * model answered.
 *
 * This is not optional instrumentation. Two separate bugs on this stack were only
 * ever caught because a table like this existed: a JSON schema that the MLX runner
 * silently ignored (replies came back as prose), and a num_predict cap that cut
 * 97% of replies off mid-object. Both looked like parse bugs from the outside and
 * were invisible without the raw reply and its length sitting in a table.
 */

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;

struct LlmJournalRecord
{
    ObjectGuid botGuid;
    std::string botName;
    uint32 botLevel{0};
    uint32 botClass{0};
    uint32 zoneId{0};

    std::string model;
    std::string endpoint;
    uint32 numPredict{0};

    std::string prompt;
    std::string reply;       // raw, exactly as the endpoint returned it
    uint32 replyChars{0};    // cheap truncation signal; see the header comment

    bool parseOk{false};
    std::string parseError;  // empty when parseOk

    std::string action;      // the accepted directive, or empty when rejected
    uint32 chosenZoneId{0};
    std::string chosenZone;
    std::string reason;

    // Outcome of the directive this call replaces, so a row carries both what was
    // decided and how the previous decision actually turned out.
    std::string prevDirective;
    std::string prevOutcome;

    uint32 latencyMs{0};
};

// One remembered decision, used to build the "recent directives" block of the
// next prompt so the model can see whether its last call worked.
struct LlmHistoryEntry
{
    std::string directive;
    std::string reason;
    std::string outcome;
};

namespace LlmJournal
{
    // Idempotent CREATE TABLE IF NOT EXISTS on the playerbots database. The same
    // DDL ships as data/sql/playerbots/updates/*.sql; this call exists so that
    // journaling still works on a server whose SQL updater is disabled.
    void EnsureSchema();

    // Fire-and-forget async insert. Safe to call from the world thread.
    void Write(LlmJournalRecord const& record);

    void PushHistory(ObjectGuid guid, LlmHistoryEntry entry);
    void SetLastOutcome(ObjectGuid guid, std::string const& outcome);
    std::vector<LlmHistoryEntry> RecentHistory(ObjectGuid guid, uint32 count);
    void Forget(ObjectGuid guid);
}

#endif
