/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LLMDIRECTIVE_H
#define PLAYERBOTS_LLMDIRECTIVE_H

#include "Define.h"

#include <string>
#include <vector>

class Player;

// The whole vocabulary the slow (LLM) layer is allowed to speak. The model never
// drives the bot directly: it emits one of these verbs plus an optional zone, and
// the classical New RPG state machine is what actually walks, fights and talks.
// Anything outside this enum is rejected before it can reach the engine.
enum class LlmDirectiveAction : uint8
{
    NONE = 0,  // no directive: the bot behaves exactly like a classical bot
    QUEST,     // work the quest log (walk POIs, kill/collect objectives)
    GRIND,     // farm mobs for XP instead of questing
    TRAVEL,    // relocate to `zoneId` by taxi
    TURNIN,    // prioritise handing in quests that are already complete
    VENDOR,    // head for a town hub (vendor/repair/restock territory)
    MAX
};

char const* LlmDirectiveActionToString(LlmDirectiveAction action);

// Case-insensitive; returns NONE for anything not in the enum.
LlmDirectiveAction LlmDirectiveActionFromString(std::string const& name);

// One (zoneId, name) pair the model is allowed to name. Built on the world thread
// when the prompt is assembled and kept alongside the pending request, so the
// reply can be validated against exactly the list the model was shown.
struct LlmZoneChoice
{
    uint32 zoneId{0};
    std::string name;
};

struct LlmDirective
{
    LlmDirectiveAction action{LlmDirectiveAction::NONE};
    uint32 zoneId{0};        // 0 = no zone preference
    std::string zoneName;    // resolved name, for logs and the next prompt
    std::string reason;      // the model's own justification, truncated
    uint32 issuedAtMs{0};    // getMSTime() when it was accepted
    bool completed{false};   // satisfied before the next decision was due
    std::string outcome;     // plain-language result, fed back in the next prompt
    std::string note;        // e.g. a zone that was dropped because it was not legal

    bool IsActive() const { return action != LlmDirectiveAction::NONE && !completed; }
    std::string Describe() const;
};

namespace LlmDirectiveParser
{
    // Turns the model's raw text into a directive, or explains why it will not.
    // A reply that does not parse is a FAILURE, never something to store raw and
    // hope for: the caller drops back to plain classical behaviour and journals it.
    bool Parse(std::string const& raw, std::vector<LlmZoneChoice> const& legalZones,
               LlmDirective& out, std::string& error);
}

namespace LlmZones
{
    // Zones the bot could plausibly be sent to right now: level-appropriate and
    // reachable by taxi for its faction, plus the zone it is standing in. This is
    // the ONLY set the model may choose from, and it is enumerated in the prompt.
    std::vector<LlmZoneChoice> LegalZonesFor(Player* bot);

    // enUS zone name, empty when the id is unknown.
    std::string NameOf(uint32 zoneId);
}

#endif
