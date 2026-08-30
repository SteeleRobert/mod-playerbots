/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LlmVendor.h"

#include "Bag.h"
#include "Item.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "StatsWeightCalculator.h"

#include <nlohmann/json.hpp>

#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
    char const* UsageName(ItemUsage usage)
    {
        switch (usage)
        {
            case ITEM_USAGE_EQUIP:        return "EQUIP";
            case ITEM_USAGE_REPLACE:      return "REPLACE";
            case ITEM_USAGE_BAD_EQUIP:    return "BAD_EQUIP";
            case ITEM_USAGE_BROKEN_EQUIP: return "BROKEN_EQUIP";
            case ITEM_USAGE_QUEST:        return "QUEST";
            case ITEM_USAGE_SKILL:        return "SKILL";
            case ITEM_USAGE_USE:          return "USE";
            case ITEM_USAGE_GUILD_TASK:   return "GUILD_TASK";
            case ITEM_USAGE_DISENCHANT:   return "DISENCHANT";
            case ITEM_USAGE_AH:           return "AH";
            case ITEM_USAGE_KEEP:         return "KEEP";
            case ITEM_USAGE_VENDOR:       return "VENDOR";
            case ITEM_USAGE_AMMO:         return "AMMO";
            default:                      return "NONE";
        }
    }

    std::string Sanitized(std::string const& value)
    {
        std::string out;
        out.reserve(std::min<size_t>(value.size(), 60));
        for (char c : value)
        {
            if (out.size() == 60)
                break;
            out.push_back((c == '\n' || c == '\r' || c == '"' || c == '\\') ? ' ' : c);
        }
        return out;
    }

    void AppendItem(Player* bot, PlayerbotAI* botAI, Item* item, StatsWeightCalculator& calculator,
                    std::ostringstream& prompt, std::vector<LlmVendorItem>& offered)
    {
        if (!item || !item->GetTemplate())
            return;

        ItemTemplate const* proto = item->GetTemplate();
        ItemUsage const usage = botAI->GetAiObjectContext()
                                    ->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
        std::string const token = "b" + std::to_string(item->GetBagSlot()) + "s" +
                                  std::to_string(item->GetSlot());
        bool const sellAllowed = LlmVendor::IsSellAllowed(usage);

        offered.push_back({token, item->GetGUID(), usage, sellAllowed});
        prompt << "- " << token << ": item_id=" << item->GetEntry()
               << ", name=" << Sanitized(proto->Name1)
               << ", count=" << item->GetCount()
               << ", quality=" << uint32(proto->Quality)
               << ", usage=" << UsageName(usage)
               << ", spec_score=" << calculator.CalculateItem(
                      proto->ItemId, item->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID))
               << ", sell_value=" << (proto->SellPrice * item->GetCount())
               << ", soulbound=" << (item->IsSoulBound() ? "yes" : "no")
               << ", sell_allowed=" << (sellAllowed ? "yes" : "no") << "\n";
    }
}

namespace LlmVendor
{
    bool IsSellAllowed(ItemUsage usage)
    {
        // QUEST and KEEP are deliberately not merely discouraged: they never enter
        // the executable set. Upgrades, consumables, profession goods and ammo are
        // protected the same way. A false negative costs one bag slot; a false
        // positive destroys an item.
        return usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH ||
               usage == ITEM_USAGE_DISENCHANT || usage == ITEM_USAGE_BAD_EQUIP;
    }

    std::string BuildPrompt(Player* bot, PlayerbotAI* botAI, std::vector<LlmVendorItem>& offered,
                            uint32& repairCost)
    {
        offered.clear();
        if (!bot || !botAI)
            return "";

        repairCost = botAI->GetAiObjectContext()->GetValue<uint32>("repair cost")->Get();
        uint8 const durability = botAI->GetAiObjectContext()->GetValue<uint8>("durability")->Get();

        std::ostringstream prompt;
        prompt << "You are the vendor agent for one World of Warcraft bot. The strategic agent has already "
                  "decided to visit town; the classical engine is walking to the nearest real vendor. Decide "
                  "only the trade actions to perform on arrival. Be conservative: preserve plausible upgrades "
                  "and future-spec gear. Free bag space is useful, and ordinary vendor trash should normally go.\n\n"
               << "Bot: level=" << uint32(bot->GetLevel()) << ", class=" << uint32(bot->getClass())
               << ", money_copper=" << bot->GetMoney()
               << ", free_bag_slots=" << bot->GetFreeInventorySpace()
               << ", durability_percent=" << uint32(durability)
               << ", repair_cost_copper=" << repairCost
               << ", repair_affordable=" << (repairCost && repairCost <= bot->GetMoney() ? "yes" : "no")
               << "\n\nBag items (tokens are the only legal values in sell):\n";

        StatsWeightCalculator calculator(bot);
        calculator.SetItemSetBonus(false);
        calculator.SetOverflowPenalty(false);

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            AppendItem(bot, botAI, bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot), calculator, prompt, offered);

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = bot->GetBagByPos(bagSlot);
            if (!bag)
                continue;
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                AppendItem(bot, botAI, bag->GetItemByPos(slot), calculator, prompt, offered);
        }

        prompt << "\nReturn exactly one JSON object and no prose, with exactly these fields:\n"
                  "{\"sell\":[\"b0s23\"],\"repair\":true,\"restock\":true,\"reason\":\"brief reason\"}\n"
                  "sell must contain only tokens marked sell_allowed=yes. Never sell QUEST or KEEP items. "
                  "Use [] when uncertain. repair may be true only when repair_affordable=yes. restock means "
                  "use the existing vendor-buy logic for useful food, ammo, consumables, or upgrades; it may "
                  "still buy nothing. All four fields are required.";
        return prompt.str();
    }

    bool Parse(std::string const& raw, std::vector<LlmVendorItem> const& offered, bool repairOffered,
               LlmVendorPlan& out, std::string& error)
    {
        out = LlmVendorPlan();
        if (raw.empty())
        {
            error = "empty reply";
            return false;
        }

        size_t const first = raw.find('{');
        size_t const last = raw.rfind('}');
        if (first == std::string::npos || last == std::string::npos || last <= first)
        {
            error = "no JSON object in reply (" + std::to_string(raw.size()) + " chars)";
            return false;
        }

        nlohmann::json doc;
        try
        {
            doc = nlohmann::json::parse(raw.substr(first, last - first + 1));
        }
        catch (std::exception const& e)
        {
            error = std::string("invalid JSON: ") + e.what();
            return false;
        }

        if (!doc.is_object() || doc.size() != 4 || !doc.contains("sell") || !doc["sell"].is_array() ||
            !doc.contains("repair") || !doc["repair"].is_boolean() ||
            !doc.contains("restock") || !doc["restock"].is_boolean() ||
            !doc.contains("reason") || !doc["reason"].is_string())
        {
            error = "reply must contain exactly sell(array), repair(bool), restock(bool), reason(string)";
            return false;
        }

        std::unordered_map<std::string, LlmVendorItem const*> legal;
        for (LlmVendorItem const& item : offered)
            legal[item.token] = &item;

        std::unordered_set<std::string> seen;
        for (nlohmann::json const& value : doc["sell"])
        {
            if (!value.is_string())
            {
                error = "every sell entry must be a string token";
                return false;
            }
            std::string const token = value.get<std::string>();
            auto const it = legal.find(token);
            if (it == legal.end() || !it->second->sellAllowed || !IsSellAllowed(it->second->usage))
            {
                error = "sell token '" + token.substr(0, 24) + "' is not in the offered sellable set";
                return false;
            }
            if (seen.insert(token).second)
                out.sell.push_back(it->second->guid);
        }

        out.repair = doc["repair"].get<bool>();
        if (out.repair && !repairOffered)
        {
            error = "repair=true was not offered (nothing to repair or insufficient money)";
            return false;
        }
        out.restock = doc["restock"].get<bool>();
        out.reason = doc["reason"].get<std::string>().substr(0, 512);
        return true;
    }
}
