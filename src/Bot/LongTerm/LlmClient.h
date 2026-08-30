/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LLMCLIENT_H
#define PLAYERBOTS_LLMCLIENT_H

/*
 * Asynchronous, Ollama-compatible HTTP client for the slow strategic layer.
 *
 * The world thread NEVER blocks on the model. Dispatch() hands the prompt to a
 * detached worker and returns immediately; the worker touches nothing but its own
 * strings (no Player*, no world state - the bot may log out, and the map may be
 * pathfinding, while the worker sits in recv()). Finished replies are parked in a
 * queue that the world thread drains on its own tick.
 */

#include "Define.h"
#include "ObjectGuid.h"

#include <string>

struct LlmReply
{
    ObjectGuid guid;
    std::string prompt;
    std::string raw;      // the model's own text, unwrapped from the API envelope
    std::string error;    // transport/endpoint failure; empty on success
    uint32 latencyMs{0};
};

namespace LlmClient
{
    // Returns false when the global in-flight cap is reached, i.e. the caller must
    // try again next tick rather than pile another socket onto a slow endpoint.
    bool Dispatch(ObjectGuid guid, std::string const& prompt);

    // World thread: hand back this bot's finished reply, if any.
    bool TakeReply(ObjectGuid guid, LlmReply& out);

    // Bot logged out / was destroyed: throw away anything already queued for it.
    void DropPending(ObjectGuid guid);

    uint32 InFlight();
}

#endif
