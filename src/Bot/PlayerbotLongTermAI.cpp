#include "PlayerbotLongTermAI.h"
#include "LongTerm/LlmJournal.h"
#include "LongTerm/LlmPrompt.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>

using json = nlohmann::json;

namespace
{
    // Parser/transport messages can carry the offending JSON, braces included, and
    // the log sink runs its own format pass over the finished message.
    std::string LogSafe(std::string const& in)
    {
        std::string out;
        out.reserve(in.size());
        for (char c : in)
            out.push_back((c == '{' || c == '}') ? ' ' : c);
        return out;
    }
}

void FunctionToolRegistry::AddTool(std::string name, std::unique_ptr<FunctionTool> handler)
{
    tools[name] = std::move(handler);
}

json FunctionToolRegistry::Handle(std::string name, json params)
{
    auto it = tools.find(name);
    if (it != tools.end())
    {
        return it->second->Handle(params);
    }
    return json::object();
}

PlayerbotLongTermAI::PlayerbotLongTermAI()
    : bot(nullptr), _timeLastUpdate(0)
{
    functionToolRegistry.AddTool("GodFunctionTool", std::make_unique<GodFunctionTool>());
}

PlayerbotLongTermAI::PlayerbotLongTermAI(Player* bot)
    : bot(bot), _timeLastUpdate(0)
{
    functionToolRegistry.AddTool("GodFunctionTool", std::make_unique<GodFunctionTool>());
}

PlayerbotLongTermAI::~PlayerbotLongTermAI()
{
    if (bot)
    {
        // A worker may still be blocked on the endpoint for this bot; make sure its
        // reply is never handed to a stale object.
        LlmClient::DropPending(bot->GetGUID());
        LlmJournal::Forget(bot->GetGUID());
        PlayerbotsMgr::instance().RemovePlayerbotLongTermAI(bot->GetGUID());
    }
}

bool PlayerbotLongTermAI::ComputeOptIn() const
{
    if (!bot)
        return false;

    std::string const name = bot->GetName();
    for (std::string const& optedIn : sPlayerbotAIConfig.llmDirectiveBotNames)
        if (optedIn == name)
            return true;

    uint32 const percent = sPlayerbotAIConfig.llmDirectiveRandomBotPercent;
    if (!percent)
        return false;

    if (!sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;

    // Deterministic per character, so an A/B split stays the same split across
    // restarts instead of reshuffling every login.
    return (bot->GetGUID().GetCounter() % 100) < percent;
}

bool PlayerbotLongTermAI::IsDirectiveLayerActive()
{
    if (!sPlayerbotAIConfig.llmDirectiveEnabled || !bot)
        return false;

    if (_optIn < 0)
        _optIn = ComputeOptIn() ? 1 : 0;

    return _optIn == 1;
}

uint32 PlayerbotLongTermAI::GetDirectiveZoneId() const
{
    return _directive.IsActive() ? _directive.zoneId : 0;
}

bool PlayerbotLongTermAI::PrefersCompletedQuests() const
{
    return _directive.IsActive() && _directive.action == LlmDirectiveAction::TURNIN;
}

void PlayerbotLongTermAI::NoteDirectiveApplied(NewRpgStatus /*status*/)
{
    ++_directiveApplyCount;
}

std::vector<NewRpgStatus> PlayerbotLongTermAI::GetPreferredRpgStatuses()
{
    std::vector<NewRpgStatus> preferred;
    if (!IsDirectiveLayerActive() || !_directive.IsActive())
        return preferred;

    // Anything asked for in another zone needs the bot to get there first, and the
    // only cross-zone transport the New RPG machine has is the taxi network.
    if (_directive.zoneId && bot->GetZoneId() != _directive.zoneId)
        preferred.push_back(RPG_TRAVEL_FLIGHT);

    switch (_directive.action)
    {
        case LlmDirectiveAction::QUEST:
        case LlmDirectiveAction::TURNIN:
            preferred.push_back(RPG_DO_QUEST);
            break;
        case LlmDirectiveAction::GRIND:
            preferred.push_back(RPG_GO_GRIND);
            preferred.push_back(RPG_WANDER_RANDOM);
            break;
        case LlmDirectiveAction::VENDOR:
            // New RPG has no vendor/repair behaviour of its own; the closest honest
            // execution is "go stand in a town hub", which is where the vendors,
            // repair NPCs and quest givers all are.
            preferred.push_back(RPG_GO_CAMP);
            break;
        case LlmDirectiveAction::TRAVEL:
            break;  // already pushed above
        default:
            break;
    }

    return preferred;
}

void PlayerbotLongTermAI::UpdateAI(uint32 /*elapsed*/, bool /*minimal*/)
{
    // Cheapest possible exit for the 100% case: the feature is off, so a classical
    // bot does one boolean test more than it did before and nothing else.
    if (!sPlayerbotAIConfig.llmDirectiveEnabled)
        return;

    if (!bot || !bot->GetSession() || !bot->IsInWorld() || bot->IsBeingTeleported() ||
        bot->GetSession()->isLogingOut() || bot->IsDuringRemoveFromWorld())
        return;

    if (!IsDirectiveLayerActive())
        return;

    // Deaths are a signal the model should see; sample the transition rather than
    // the state so a five-minute corpse run counts once.
    bool const dead = !bot->IsAlive();
    if (dead && !_wasDead)
        ++_deathsSinceLastDecision;
    _wasDead = dead;

    uint32 const now = getMSTime();

    // A reply that came back since the last tick is applied here, on the world
    // thread, where touching bot state is legal.
    if (_pending)
    {
        LlmReply reply;
        if (LlmClient::TakeReply(bot->GetGUID(), reply))
            ConsumeReply(reply);
    }

    CheckDirectiveCompletion();

    if (_nextDecisionMs == 0)
    {
        // Stagger the very first decision across the whole interval. Without this,
        // fifty bots that logged in together would all fire in the same tick
        // forever after.
        _nextDecisionMs = now + urand(0, sPlayerbotAIConfig.llmDirectiveIntervalSeconds * 1000);
        return;
    }

    if (_pending || now < _nextDecisionMs)
        return;

    // Asking a corpse what it wants to do for the next five minutes wastes a call;
    // the dead engine handles release and resurrect on its own.
    if (dead)
    {
        _nextDecisionMs = now + 30 * 1000;
        return;
    }

    RequestDecision(now);
}

void PlayerbotLongTermAI::RequestDecision(uint32 now)
{
    std::vector<LlmZoneChoice> zones;
    std::string prompt = LlmPrompt::BuildDecisionPrompt(bot, _deathsSinceLastDecision, zones);
    if (prompt.empty())
    {
        _nextDecisionMs = now + 30 * 1000;
        return;
    }

    if (!LlmClient::Dispatch(bot->GetGUID(), prompt))
    {
        // At the global in-flight cap. Back off briefly rather than dropping this
        // bot's turn entirely, and never queue behind a slow endpoint.
        _nextDecisionMs = now + 5 * 1000;
        return;
    }

    _pending = true;
    _pendingPrompt = std::move(prompt);
    _pendingZones = std::move(zones);

    uint32 const jitter = sPlayerbotAIConfig.llmDirectiveJitterSeconds;
    _nextDecisionMs = now + sPlayerbotAIConfig.llmDirectiveIntervalSeconds * 1000 +
                      (jitter ? urand(0, jitter * 1000) : 0);
}

void PlayerbotLongTermAI::ConsumeReply(LlmReply const& reply)
{
    _pending = false;

    LlmJournalRecord record;
    record.botGuid = bot->GetGUID();
    record.botName = bot->GetName();
    record.botLevel = bot->GetLevel();
    record.botClass = bot->getClass();
    record.zoneId = bot->GetZoneId();
    record.model = sPlayerbotAIConfig.llmDirectiveModel;
    record.endpoint = sPlayerbotAIConfig.llmDirectiveUrl;
    record.numPredict = sPlayerbotAIConfig.llmDirectiveNumPredict;
    record.prompt = _pendingPrompt;
    record.reply = reply.raw;
    record.replyChars = static_cast<uint32>(reply.raw.size());
    record.latencyMs = reply.latencyMs;
    record.prevDirective = _directive.action != LlmDirectiveAction::NONE ? _directive.Describe() : "";
    record.prevOutcome = _directive.action != LlmDirectiveAction::NONE ? SummariseDirectiveOutcome() : "";

    // Close the book on the previous directive before the new one replaces it, so
    // the history the model sees always has an outcome attached.
    if (_directive.action != LlmDirectiveAction::NONE)
        LlmJournal::SetLastOutcome(bot->GetGUID(), record.prevOutcome);

    LlmDirective parsed;
    std::string error;

    if (!reply.error.empty())
    {
        error = reply.error;
    }
    else if (!LlmDirectiveParser::Parse(reply.raw, _pendingZones, parsed, error))
    {
        // Parse failure is a failure, full stop. Nothing is stored as a directive,
        // and the bot spends the next interval as a plain classical bot.
    }

    record.parseOk = error.empty();
    record.parseError = error;

    if (record.parseOk)
    {
        parsed.issuedAtMs = getMSTime();
        record.action = LlmDirectiveActionToString(parsed.action);
        record.chosenZoneId = parsed.zoneId;
        record.chosenZone = parsed.zoneName;
        record.reason = parsed.reason;
        if (!parsed.note.empty())
            record.parseError = "note: " + parsed.note;  // accepted, but worth seeing in the table

        _directive = parsed;
        _directiveApplyCount = 0;
        _atIssueQuestRewarded = 0;
        _atIssueQuestAccepted = 0;
        _atIssueLevel = bot->GetLevel();
        _atIssueZoneId = bot->GetZoneId();

        if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        {
            _atIssueQuestRewarded = botAI->rpgStatistic.questRewarded;
            _atIssueQuestAccepted = botAI->rpgStatistic.questAccepted;
        }

        // Send the bot back to IDLE so the New RPG machine re-rolls its status on the
        // next tick and our hook gets first refusal. Without this the directive waits
        // for whatever the bot is already doing to time out - up to 30 minutes for
        // DO_QUEST - and usually expires unused.
        if (sPlayerbotAIConfig.llmDirectivePreempt)
        {
            if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
            {
                NewRpgStatus const current = botAI->rpgInfo.GetStatus();
                std::vector<NewRpgStatus> const preferred = GetPreferredRpgStatuses();
                bool const alreadyDoingIt =
                    std::find(preferred.begin(), preferred.end(), current) != preferred.end();

                // Never interrupt a taxi hop (the bot is mid-air and the flight is the
                // only cross-zone transport there is), and never yank it out of combat.
                bool const busyUninterruptible =
                    current == RPG_TRAVEL_FLIGHT || bot->IsInCombat() || !bot->IsAlive();

                if (!alreadyDoingIt && !busyUninterruptible)
                    botAI->rpgInfo.ChangeToIdle();
            }
        }

        LlmHistoryEntry entry;
        entry.directive = _directive.Describe();
        entry.reason = _directive.reason;
        LlmJournal::PushHistory(bot->GetGUID(), std::move(entry));

        if (sPlayerbotAIConfig.llmDirectiveDebug)
            LOG_INFO("playerbots", "[LlmDirective] {} -> {} ({}ms): {}", bot->GetName(),
                     _directive.Describe(), reply.latencyMs, _directive.reason);
    }
    else
    {
        _directive = LlmDirective();
        _directiveApplyCount = 0;

        LOG_WARN("playerbots", "[LlmDirective] {} rejected reply ({} chars, {}ms): {}", bot->GetName(),
                 record.replyChars, reply.latencyMs, LogSafe(error));
    }

    _deathsSinceLastDecision = 0;
    _pendingPrompt.clear();
    _pendingZones.clear();

    if (sPlayerbotAIConfig.llmDirectiveJournal)
        LlmJournal::Write(record);
}

void PlayerbotLongTermAI::CheckDirectiveCompletion()
{
    if (!_directive.IsActive())
        return;

    switch (_directive.action)
    {
        case LlmDirectiveAction::TRAVEL:
            if (_directive.zoneId && bot->GetZoneId() == _directive.zoneId)
                RetireDirective("arrived in " + _directive.zoneName);
            break;
        case LlmDirectiveAction::TURNIN:
        {
            bool anyComplete = false;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE && !anyComplete; ++slot)
            {
                uint32 const questId = bot->GetQuestSlotQuestId(slot);
                if (questId && bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                    anyComplete = true;
            }
            if (!anyComplete)
                RetireDirective("nothing left to hand in");
            break;
        }
        default:
            break;
    }

    // Belt and braces: a directive must never outlive its decision window, even if
    // a reply is lost and no new one arrives to replace it.
    if (_directive.IsActive() &&
        GetMSTimeDiffToNow(_directive.issuedAtMs) > sPlayerbotAIConfig.llmDirectiveIntervalSeconds * 3000)
    {
        RetireDirective("expired without being replaced");
    }
}

void PlayerbotLongTermAI::RetireDirective(std::string const& outcome)
{
    std::string const full = outcome + "; " + SummariseDirectiveOutcome();
    LlmJournal::SetLastOutcome(bot->GetGUID(), full);
    _directive.completed = true;
    _directive.outcome = full;
}

std::string PlayerbotLongTermAI::SummariseDirectiveOutcome() const
{
    std::ostringstream out;

    if (!_directiveApplyCount)
        out << "the engine never took it up";
    else
        out << "acted on it " << _directiveApplyCount << "x";

    if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        uint32 const rewarded = botAI->rpgStatistic.questRewarded > _atIssueQuestRewarded
                                    ? botAI->rpgStatistic.questRewarded - _atIssueQuestRewarded : 0;
        uint32 const accepted = botAI->rpgStatistic.questAccepted > _atIssueQuestAccepted
                                    ? botAI->rpgStatistic.questAccepted - _atIssueQuestAccepted : 0;
        if (rewarded)
            out << ", turned in " << rewarded << " quest(s)";
        if (accepted)
            out << ", picked up " << accepted << " quest(s)";
    }

    if (bot->GetLevel() > _atIssueLevel)
        out << ", reached level " << uint32(bot->GetLevel());

    if (bot->GetZoneId() != _atIssueZoneId)
    {
        std::string const zoneName = LlmZones::NameOf(bot->GetZoneId());
        out << ", now in " << (zoneName.empty() ? std::to_string(bot->GetZoneId()) : zoneName);
    }

    return out.str();
}

void PlayerbotLongTermAI::Decide()
{
     // Make call to AI agent here
    std::string aiResponsePayload = R"({
        "tool_calls": [
            {
                "name": "GodFunctionTool",
                "params": {
                    "someParam": "someValue"
                }
            }
        ]
    })";

    try {
        json parsedResponse = json::parse(aiResponsePayload);

        for (const auto& call : parsedResponse["tool_calls"])
        {
            std::string toolName = call["name"];
            json toolParams = call["params"];
            json result = functionToolRegistry.Handle(toolName, toolParams);

            LOG_DEBUG("playerbots", "Tool call result: {}", result.dump(2).c_str());
        }
    } catch (const json::parse_error& e) {
        LOG_ERROR("playerbots", "JSON parse error: {}", e.what());
    }
}
