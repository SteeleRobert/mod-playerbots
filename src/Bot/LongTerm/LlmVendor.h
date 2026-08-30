/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LLMVENDOR_H
#define PLAYERBOTS_LLMVENDOR_H

#include "Define.h"
#include "ItemUsageValue.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;
class PlayerbotAI;
class Item;

struct LlmVendorItem
{
    std::string token;
    ObjectGuid guid;
    ItemUsage usage{ITEM_USAGE_NONE};
    bool sellAllowed{false};
};

struct LlmVendorPlan
{
    std::vector<ObjectGuid> sell;
    bool repair{false};
    bool restock{false};
    std::string reason;
};

namespace LlmVendor
{
    std::string BuildPrompt(Player* bot, PlayerbotAI* botAI, std::vector<LlmVendorItem>& offered,
                            uint32& repairCost);
    bool Parse(std::string const& raw, std::vector<LlmVendorItem> const& offered, bool repairOffered,
               LlmVendorPlan& out, std::string& error);
    bool IsSellAllowed(Player* bot, Item* item, ItemUsage usage);
}

#endif
