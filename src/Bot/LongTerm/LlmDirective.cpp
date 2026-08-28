/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LlmDirective.h"

#include "DBCStores.h"
#include "Player.h"
#include "TravelMgr.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace
{
    std::string ToLower(std::string const& in)
    {
        std::string out = in;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }
}

char const* LlmDirectiveActionToString(LlmDirectiveAction action)
{
    switch (action)
    {
        case LlmDirectiveAction::QUEST:  return "quest";
        case LlmDirectiveAction::GRIND:  return "grind";
        case LlmDirectiveAction::TRAVEL: return "travel";
        case LlmDirectiveAction::TURNIN: return "turnin";
        case LlmDirectiveAction::VENDOR: return "vendor";
        default:                         return "none";
    }
}

LlmDirectiveAction LlmDirectiveActionFromString(std::string const& name)
{
    std::string const lowered = ToLower(name);
    for (uint8 i = 1; i < static_cast<uint8>(LlmDirectiveAction::MAX); ++i)
    {
        LlmDirectiveAction const action = static_cast<LlmDirectiveAction>(i);
        if (lowered == LlmDirectiveActionToString(action))
            return action;
    }
    return LlmDirectiveAction::NONE;
}

std::string LlmDirective::Describe() const
{
    std::string out = LlmDirectiveActionToString(action);
    if (zoneId)
        out += " -> " + (zoneName.empty() ? std::to_string(zoneId) : zoneName);
    return out;
}

namespace LlmDirectiveParser
{
    bool Parse(std::string const& raw, std::vector<LlmZoneChoice> const& legalZones,
               LlmDirective& out, std::string& error)
    {
        out = LlmDirective();

        if (raw.empty())
        {
            error = "empty reply";
            return false;
        }

        // Even with format=json a model will occasionally wrap the object in prose
        // or a code fence. Take the outermost braces and let the parser judge.
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

        if (!doc.is_object())
        {
            error = "reply is not a JSON object";
            return false;
        }

        // Tolerate exactly one level of wrapping ({"directive": {...}}) and nothing
        // more inventive than that.
        if (!doc.contains("action") && doc.size() == 1 && doc.begin().value().is_object())
            doc = doc.begin().value();

        if (!doc.contains("action") || !doc["action"].is_string())
        {
            error = "missing string field 'action'";
            return false;
        }

        std::string const actionName = doc["action"].get<std::string>();
        LlmDirectiveAction const action = LlmDirectiveActionFromString(actionName);
        if (action == LlmDirectiveAction::NONE)
        {
            error = "action '" + actionName.substr(0, 32) + "' is not one of quest|grind|travel|turnin|vendor";
            return false;
        }
        out.action = action;

        std::string requestedZone;
        if (doc.contains("zone"))
        {
            if (doc["zone"].is_string())
                requestedZone = doc["zone"].get<std::string>();
            else if (doc["zone"].is_number_unsigned())
                requestedZone = std::to_string(doc["zone"].get<uint32>());
        }

        // Trim; models pad zone names with spaces surprisingly often.
        size_t const zoneBegin = requestedZone.find_first_not_of(" \t\r\n");
        size_t const zoneEnd = requestedZone.find_last_not_of(" \t\r\n");
        requestedZone = (zoneBegin == std::string::npos) ? "" : requestedZone.substr(zoneBegin, zoneEnd - zoneBegin + 1);

        if (!requestedZone.empty() && requestedZone != "none" && requestedZone != "\"\"")
        {
            std::string const wanted = ToLower(requestedZone);
            for (LlmZoneChoice const& choice : legalZones)
            {
                if (ToLower(choice.name) == wanted || std::to_string(choice.zoneId) == wanted)
                {
                    out.zoneId = choice.zoneId;
                    out.zoneName = choice.name;
                    break;
                }
            }

            if (!out.zoneId)
            {
                // A travel order to a zone that was never offered is meaningless, so
                // the whole reply is rejected. For the other verbs the zone is an
                // optional extra: drop it, keep the verb, and say so in the journal.
                if (action == LlmDirectiveAction::TRAVEL)
                {
                    error = "travel zone '" + requestedZone.substr(0, 48) + "' is not in the offered list";
                    return false;
                }
                out.note = "ignored out-of-list zone '" + requestedZone.substr(0, 48) + "'";
            }
        }

        if (action == LlmDirectiveAction::TRAVEL && !out.zoneId)
        {
            error = "travel requires a zone from the offered list";
            return false;
        }

        if (doc.contains("reason") && doc["reason"].is_string())
        {
            // The reason is model-authored free text that ends up in fmt log calls
            // and in the next prompt, so strip the characters that would break
            // either one before it is stored anywhere.
            std::string const rawReason = doc["reason"].get<std::string>();
            out.reason.reserve(std::min<size_t>(rawReason.size(), 200));
            for (char c : rawReason)
            {
                if (out.reason.size() >= 200)
                    break;
                if (c == '{' || c == '}' || c == '"' || c == '\\')
                    out.reason.push_back(' ');
                else if (c == '\n' || c == '\r' || c == '\t')
                    out.reason.push_back(' ');
                else
                    out.reason.push_back(c);
            }
        }

        return true;
    }
}

namespace LlmZones
{
    std::string NameOf(uint32 zoneId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
        if (!area || !area->area_name[LOCALE_enUS])
            return "";
        return area->area_name[LOCALE_enUS];
    }

    std::vector<LlmZoneChoice> LegalZonesFor(Player* bot)
    {
        std::vector<LlmZoneChoice> result;
        if (!bot)
            return result;

        // Hard cap so a level bracket that happens to span half the world cannot
        // blow the prompt (and with it the reply) past the token budget.
        constexpr size_t MAX_ZONES = 24;

        std::unordered_set<uint32> seen;
        auto push = [&](uint32 zoneId)
        {
            if (!zoneId || result.size() >= MAX_ZONES || !seen.insert(zoneId).second)
                return;
            std::string name = NameOf(zoneId);
            if (name.empty())
                return;
            result.push_back({zoneId, std::move(name)});
        };

        // The zone the bot is standing in is always legal: "stay put" has to be
        // expressible, otherwise every reply is a relocation.
        push(bot->GetZoneId());

        for (uint32 zoneId : sTravelMgr.GetLevelAppropriateZones(bot))
            push(zoneId);

        return result;
    }
}
