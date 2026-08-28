/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LLMPROMPT_H
#define PLAYERBOTS_LLMPROMPT_H

#include "LlmDirective.h"

#include <string>
#include <vector>

class Player;

namespace LlmPrompt
{
    // Assembles the whole decision prompt. MUST be called on the world thread: it
    // reads live quest state, nearby-unit values and inventory.
    //
    // `legalZones` is filled with exactly the zone list the prompt offers, so the
    // reply can later be validated against what the model was actually shown.
    std::string BuildDecisionPrompt(Player* bot, uint32 deathsSinceLastDecision,
                                    std::vector<LlmZoneChoice>& legalZones);
}

#endif
