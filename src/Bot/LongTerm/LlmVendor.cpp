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
#include "RandomItemMgr.h"
#include "StatsWeightCalculator.h"

#include <nlohmann/json.hpp>

#include <sstream>

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

    bool IsNeutralEquipmentSlot(uint32 inventoryType)
    {
        switch (inventoryType)
        {
            case INVTYPE_NECK:
            case INVTYPE_CLOAK:
            case INVTYPE_FINGER:
            case INVTYPE_TRINKET:
            case INVTYPE_HOLDABLE:
            case INVTYPE_RELIC:
                return true;
            default:
                return false;
        }
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
        bool const sellAllowed = LlmVendor::IsSellAllowed(bot, item, usage);

        offered.push_back({token, item->GetGUID(), usage, sellAllowed});
        prompt << "- " << token << ": item_id=" << item->GetEntry()
               << ", name=" << Sanitized(proto->Name1)
               << ", count=" << item->GetCount()
               << ", quality=" << uint32(proto->Quality)
               << ", required_level=" << proto->RequiredLevel
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
    bool IsSellAllowed(Player* bot, Item* item, ItemUsage usage)
    {
        if (!bot || !item || !item->GetTemplate() || !item->GetTemplate()->SellPrice)
            return false;

        // Until professions and the AH exist, all saleable non-quest inventory is
        // vendor fodder except equipment this class can use now or after levelling.
        // Quest protection is absolute even when an item would otherwise look like
        // ordinary trade goods.
        if (usage == ITEM_USAGE_QUEST)
            return false;

        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Class == ITEM_CLASS_WEAPON &&
            sRandomItemMgr.CanEquipWeapon(proto, bot->getClass()))
            return false;

        if (proto->Class == ITEM_CLASS_ARMOR)
        {
            bool const classAllowed = (proto->AllowableClass & bot->getClassMask()) != 0;
            bool const usableArmor =
                sRandomItemMgr.CanEquipArmor(proto, bot->getClass(), bot->GetLevel()) ||
                sRandomItemMgr.CanEquipArmor(proto, bot->getClass(), DEFAULT_MAX_LEVEL) ||
                (classAllowed && IsNeutralEquipmentSlot(proto->InventoryType));
            if (usableArmor)
                return false;
        }

        // ItemUsage knows about currently usable bags and unusual equippables that
        // are not represented by the normal weapon/armor class checks.
        if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE ||
            usage == ITEM_USAGE_BAD_EQUIP || usage == ITEM_USAGE_BROKEN_EQUIP)
            return false;

        return true;
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
                  "only repair and restock on arrival. Professions and the auction house are not implemented. "
                  "The engine has already marked every saleable non-quest, non-usable-gear item for sale.\n\n"
               << "Bot: level=" << uint32(bot->GetLevel()) << ", class=" << uint32(bot->getClass())
               << ", money_copper=" << bot->GetMoney()
               << ", free_bag_slots=" << bot->GetFreeInventorySpace()
               << ", durability_percent=" << uint32(durability)
               << ", repair_cost_copper=" << repairCost
               << ", repair_affordable=" << (repairCost && repairCost <= bot->GetMoney() ? "yes" : "no")
               << "\n\nBag items (sell_allowed is determined mechanically):\n";

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
                  "{\"sell_all_allowed\":true,\"repair\":true,\"restock\":true,\"reason\":\"brief reason\"}\n"
                  "sell_all_allowed must be true; the engine owns the protected-item rules. "
                  "repair may be true only when repair_affordable=yes. restock means "
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

        if (!doc.is_object() || doc.size() != 4 || !doc.contains("sell_all_allowed") ||
            !doc["sell_all_allowed"].is_boolean() || !doc["sell_all_allowed"].get<bool>() ||
            !doc.contains("repair") || !doc["repair"].is_boolean() ||
            !doc.contains("restock") || !doc["restock"].is_boolean() ||
            !doc.contains("reason") || !doc["reason"].is_string())
        {
            error = "reply must contain exactly sell_all_allowed(true), repair(bool), restock(bool), reason(string)";
            return false;
        }

        for (LlmVendorItem const& item : offered)
            if (item.sellAllowed)
                out.sell.push_back(item.guid);

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
