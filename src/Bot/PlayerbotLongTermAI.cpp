#include "PlayerbotLongTermAI.h"
#include "LongTerm/LlmJournal.h"
#include "LongTerm/LlmPrompt.h"
#include "LongTerm/LlmTelemetry.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>
#include <variant>

using json = nlohmann::json;

namespace
{
    constexpr char const* STRATEGIC_AGENT_ID = "strategic";
    constexpr char const* VENDOR_AGENT_ID = "vendor";
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

bool PlayerbotLongTermAI::IsHonestBot(Player* bot)
{
    if (!bot || !sPlayerbotAIConfig.llmDirectiveEnabled || !sPlayerbotAIConfig.llmDirectiveNoCheating)
        return false;

    PlayerbotLongTermAI* longTermAI = PlayerbotsMgr::instance().GetPlayerbotLongTermAI(bot);
    return longTermAI && longTermAI->IsDirectiveLayerActive();
}

uint32 PlayerbotLongTermAI::GetDirectiveZoneId() const
{
    return _directive.IsActive() ? _directive.zoneId : 0;
}

uint32 PlayerbotLongTermAI::GetDirectiveQuestId() const
{
    return _directive.IsActive() ? _directive.questId : 0;
}

bool PlayerbotLongTermAI::PrefersCompletedQuests() const
{
    return _directive.IsActive() && _directive.action == LlmDirectiveAction::TURNIN;
}

void PlayerbotLongTermAI::NoteDirectiveApplied(NewRpgStatus /*status*/)
{
    ++_directiveApplyCount;
}

bool PlayerbotLongTermAI::BeginVendorHandoff()
{
    if (!_directive.IsActive() || _directive.action != LlmDirectiveAction::VENDOR)
        return false;
    if (_vendorHandoff)
        return true;
    // A canceled vendor worker still has to be drained and journalled. Do not
    // create a second request with the same agent key until that has happened.
    if (_vendorPending)
        return false;

    uint32 repairCost = 0;
    std::vector<LlmVendorItem> offered;
    std::string prompt = LlmVendor::BuildPrompt(bot, PlayerbotsMgr::instance().GetPlayerbotAI(bot),
                                                offered, repairCost);
    if (prompt.empty() || !LlmClient::Dispatch(bot->GetGUID(), VENDOR_AGENT_ID, prompt))
        return false;

    _vendorHandoff = true;
    _vendorPending = true;
    _vendorPlanReady = false;
    _vendorRepairOffered = repairCost > 0 && repairCost <= bot->GetMoney();
    _vendorPrompt = std::move(prompt);
    _vendorItems = std::move(offered);
    _vendorPlan = LlmVendorPlan();
    return true;
}

bool PlayerbotLongTermAI::IsVendorHandoffActive() const
{
    return _vendorHandoff && _directive.IsActive() && _directive.action == LlmDirectiveAction::VENDOR;
}

bool PlayerbotLongTermAI::GetVendorPlan(LlmVendorPlan& out) const
{
    if (!IsVendorHandoffActive() || !_vendorPlanReady)
        return false;
    out = _vendorPlan;
    return true;
}

void PlayerbotLongTermAI::CompleteVendorHandoff(std::string const& outcome)
{
    if (!IsVendorHandoffActive())
        return;

    _vendorHandoff = false;
    _vendorPlanReady = false;
    _vendorRepairOffered = false;
    // Normally the reply has already been consumed. If a timeout canceled an
    // unusually slow request, retain its evidence until it arrives so every LLM
    // call is still journalled; the agent key prevents it colliding with strategy.
    if (!_vendorPending)
    {
        _vendorPrompt.clear();
        _vendorItems.clear();
    }
    _vendorPlan = LlmVendorPlan();
    RetireDirective("vendor service complete: " + outcome);
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
            // DO_QUEST needs a quest in the log with usable POI data. A bot with an
            // empty log cannot satisfy "quest" that way, and the model says so itself
            // ("No quests active; need to accept new ones"). Visiting nearby NPCs is
            // how this engine acquires and hands in quests, so it is the honest
            // second choice rather than dropping to an unrelated random roll.
            preferred.push_back(RPG_WANDER_NPC);
            break;
        case LlmDirectiveAction::GRIND:
            preferred.push_back(RPG_GO_GRIND);
            preferred.push_back(RPG_WANDER_RANDOM);
            break;
        case LlmDirectiveAction::VENDOR:
            // The strategic half gets the bot to a town hub. On arrival GO_CAMP
            // explicitly hands off to the separately prompted vendor agent and
            // the classical RPG_VENDOR execution state.
            preferred.push_back(RPG_GO_CAMP);
            preferred.push_back(RPG_WANDER_NPC);
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
    {
        ++_deathsSinceLastDecision;
        LlmTelemetry::RecordEvent(bot, LlmTelemetry::EVENT_DIED);

        // Whatever it was doing is moot now. Mark the decision due; the dead-bot
        // guard below still holds it back until the bot is on its feet again, so
        // this asks "you just died, now what" rather than prompting a corpse.
        if (sPlayerbotAIConfig.llmDirectiveReactToInterrupts && _directive.IsActive())
            RetireDirective("you died");
        else if (sPlayerbotAIConfig.llmDirectiveReactToInterrupts)
            BringDecisionForward();
    }
    _wasDead = dead;

    // Level-ups and quest turn-ins, for the dashboard's counters and markers.
    // Primed on the first tick so a fresh login does not report every quest the
    // bot has ever handed in as having just happened.
    if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        uint32 const level = bot->GetLevel();
        uint32 const rewarded = botAI->rpgStatistic.questRewarded;

        if (!_telemetryPrimed)
        {
            _telemetryLevel = level;
            _telemetryQuestRewarded = rewarded;
            _telemetryPrimed = true;
        }
        else
        {
            if (level > _telemetryLevel)
            {
                LlmTelemetry::RecordEvent(bot, LlmTelemetry::EVENT_LEVELED_UP,
                                          "{\"level\":" + std::to_string(level) + "}");
                _telemetryLevel = level;
            }
            for (uint32 i = _telemetryQuestRewarded; i < rewarded; ++i)
                LlmTelemetry::RecordEvent(bot, LlmTelemetry::EVENT_QUEST_DONE);
            _telemetryQuestRewarded = rewarded;
        }
    }

    uint32 const now = getMSTime();

    // A reply that came back since the last tick is applied here, on the world
    // thread, where touching bot state is legal.
    if (_pending)
    {
        LlmReply reply;
        if (LlmClient::TakeReply(bot->GetGUID(), STRATEGIC_AGENT_ID, reply))
            ConsumeReply(reply);
    }

    if (_vendorPending)
    {
        LlmReply reply;
        if (LlmClient::TakeReply(bot->GetGUID(), VENDOR_AGENT_ID, reply))
            ConsumeVendorReply(reply);
    }

    CheckDirectiveCompletion();

    // A directive whose status the bot is already in is being followed, even though
    // no status roll ever happened. Record that, or the outcome fed back to the
    // model reads "the engine never took it up" while the bot does exactly as told.
    if (_directive.IsActive() && !_directiveAligned)
    {
        if (PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        {
            NewRpgStatus const current = botAI->rpgInfo.GetStatus();
            std::vector<NewRpgStatus> const preferred = GetPreferredRpgStatuses();
            if (std::find(preferred.begin(), preferred.end(), current) != preferred.end())
                _directiveAligned = true;
        }
    }

    if (_nextDecisionMs == 0)
    {
        // Stagger the very first decision across the whole interval. Without this,
        // fifty bots that logged in together would all fire in the same tick
        // forever after.
        _nextDecisionMs = now + urand(0, sPlayerbotAIConfig.llmDirectiveIntervalSeconds * 1000);
        return;
    }

    if (_pending || _vendorHandoff || now < _nextDecisionMs)
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

void PlayerbotLongTermAI::ConsumeVendorReply(LlmReply const& reply)
{
    _vendorPending = false;

    LlmJournalRecord record;
    record.botGuid = bot->GetGUID();
    record.botName = bot->GetName();
    record.botLevel = bot->GetLevel();
    record.botClass = bot->getClass();
    record.zoneId = bot->GetZoneId();
    record.model = sPlayerbotAIConfig.llmDirectiveModel;
    record.endpoint = sPlayerbotAIConfig.llmDirectiveUrl;
    record.numPredict = sPlayerbotAIConfig.llmDirectiveNumPredict;
    record.prompt = _vendorPrompt;
    record.reply = reply.raw;
    record.replyChars = static_cast<uint32>(reply.raw.size());
    record.latencyMs = reply.latencyMs;
    record.action = "vendor-service";
    record.prevDirective = _directive.Describe();

    std::string error = reply.error;
    LlmVendorPlan parsed;
    if (error.empty() && !LlmVendor::Parse(reply.raw, _vendorItems, _vendorRepairOffered, parsed, error))
    {
        // A rejected plan becomes a ready no-op plan. This lets the classical
        // action leave the vendor cleanly without guessing at destructive work.
    }

    record.parseOk = error.empty();
    record.parseError = error;
    if (record.parseOk)
    {
        _vendorPlan = std::move(parsed);
        record.reason = _vendorPlan.reason;
    }
    else
    {
        _vendorPlan = LlmVendorPlan();
        LOG_WARN("playerbots", "[LlmVendor] {} rejected reply ({} chars, {}ms): {}", bot->GetName(),
                 record.replyChars, reply.latencyMs, LogSafe(error));
    }
    _vendorPlanReady = _vendorHandoff;

    if (sPlayerbotAIConfig.llmDirectiveJournal)
        LlmJournal::Write(record);
    LlmTelemetry::WriteDecision(bot, record.parseOk ? "vendor-service" : "vendor-rejected", "{}",
                                record.reason, record.parseOk, record.parseOk ? "plan accepted" : error,
                                record.latencyMs, record.prompt, record.reply);

    _vendorPrompt.clear();
    _vendorItems.clear();
}

void PlayerbotLongTermAI::RequestDecision(uint32 now)
{
    std::vector<LlmZoneChoice> zones;
    std::vector<uint32> quests;
    std::string prompt = LlmPrompt::BuildDecisionPrompt(bot, _deathsSinceLastDecision, zones, quests);
    if (prompt.empty())
    {
        _nextDecisionMs = now + 30 * 1000;
        return;
    }

    if (!LlmClient::Dispatch(bot->GetGUID(), STRATEGIC_AGENT_ID, prompt))
    {
        // At the global in-flight cap. Back off briefly rather than dropping this
        // bot's turn entirely, and never queue behind a slow endpoint.
        _nextDecisionMs = now + 5 * 1000;
        return;
    }

    _pending = true;
    _lastDecisionMs = now;
    _pendingPrompt = std::move(prompt);
    _pendingZones = std::move(zones);
    _pendingQuests = std::move(quests);

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
    // A retired directive already carries the reason it ended ("finished quest 3904",
    // "arrived in Westfall", "you died") plus its own summary. Prefer that: it is
    // what actually triggered this decision, and overwriting it with the bare
    // summary hides whether the bot finished or merely timed out.
    if (_directive.action == LlmDirectiveAction::NONE)
        record.prevOutcome = "";
    else if (!_directive.outcome.empty())
        record.prevOutcome = _directive.outcome;
    else
        record.prevOutcome = SummariseDirectiveOutcome();

    // Close the book on the previous directive before the new one replaces it, so
    // the history the model sees always has an outcome attached.
    if (_directive.action != LlmDirectiveAction::NONE && _directive.outcome.empty())
        LlmJournal::SetLastOutcome(bot->GetGUID(), record.prevOutcome);

    LlmDirective parsed;
    std::string error;

    if (!reply.error.empty())
    {
        error = reply.error;
    }
    else if (!LlmDirectiveParser::Parse(reply.raw, _pendingZones, _pendingQuests, parsed, error))
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
        _directiveAligned = false;
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

                // A bot parked at a quest POI is running the engine's own stall
                // timer: five minutes there with no progress and it blacklists the
                // quest and moves on. That timer is the ONLY escape from objectives
                // this engine cannot detect progress on - exploration, use-item and
                // reputation objectives never register - and ChangeToIdle resets it.
                //
                // With IntervalSeconds and poiStayTime both 300s we were resetting
                // it at almost exactly the moment it would have fired, so a bot could
                // sit at a dead objective forever. Measured on olab1: one bot spent
                // 33 minutes inside a 25-yard box in a mine, zero xp, zero kills,
                // re-issued the same directive seven times.
                //
                // So leave a parked bot alone. The directive still lands - at the
                // next roll, which is exactly what the stall timer triggers.
                bool parkedAtQuestPoi = false;
                if (current == RPG_DO_QUEST)
                    if (NewRpgInfo::DoQuest const* dq =
                            std::get_if<NewRpgInfo::DoQuest>(&botAI->rpgInfo.data))
                        parkedAtQuestPoi = dq->lastReachPOI != 0;

                if (!alreadyDoingIt && !busyUninterruptible && !parkedAtQuestPoi)
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
        _directiveAligned = false;

        LOG_WARN("playerbots", "[LlmDirective] {} rejected reply ({} chars, {}ms): {}", bot->GetName(),
                 record.replyChars, reply.latencyMs, LogSafe(error));
    }

    _deathsSinceLastDecision = 0;
    _pendingPrompt.clear();
    _pendingZones.clear();
    _pendingQuests.clear();

    if (sPlayerbotAIConfig.llmDirectiveJournal)
        LlmJournal::Write(record);

    // Same decision, in the shape the dashboard already knows how to read.
    LlmTelemetry::WriteDecision(bot,
                                record.parseOk ? record.action : "rejected",
                                record.chosenZone.empty() ? std::string("{}")
                                                          : ("{\"zone\":\"" + record.chosenZone + "\"}"),
                                record.reason, record.parseOk,
                                record.parseOk ? record.prevOutcome : record.parseError,
                                record.latencyMs, record.prompt, record.reply);
}

// True while the bot still holds this quest with work left on it.
bool PlayerbotLongTermAI::BotHoldsIncompleteQuest(uint32 questId) const
{
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (bot->GetQuestSlotQuestId(slot) == questId)
            return bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE;

    return false;   // no longer in the log at all
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
        case LlmDirectiveAction::QUEST:
            // A named quest whose objectives are done is finished as far as this
            // directive goes - what to do next (hand it in, start another, move on)
            // is exactly the decision worth asking for now rather than in four
            // minutes' time.
            if (_directive.questId && !BotHoldsIncompleteQuest(_directive.questId))
                RetireDirective("finished quest " + std::to_string(_directive.questId));
            break;
        case LlmDirectiveAction::TURNIN:
        {
            // A named quest is done the moment it leaves the log (rewarded).
            if (_directive.questId)
            {
                if (bot->GetQuestStatus(_directive.questId) != QUEST_STATUS_COMPLETE)
                    RetireDirective("handed in quest " + std::to_string(_directive.questId));
                break;
            }

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
    // Death or another interruption can retire a vendor directive while its
    // second-agent request is in flight. Release the state machine immediately,
    // but leave an in-flight request drainable so its journal row is not lost.
    if (_vendorHandoff)
    {
        _vendorHandoff = false;
        _vendorPlanReady = false;
        _vendorPlan = LlmVendorPlan();
        if (!_vendorPending)
        {
            _vendorPrompt.clear();
            _vendorItems.clear();
        }
    }

    std::string const full = outcome + "; " + SummariseDirectiveOutcome();
    LlmJournal::SetLastOutcome(bot->GetGUID(), full);
    _directive.completed = true;
    _directive.outcome = full;

    if (!sPlayerbotAIConfig.llmDirectiveReactToCompletion)
        return;

    // Bring the next decision forward, but never closer to the last one than the
    // configured floor. An "expired" directive is deliberately excluded: that is a
    // timeout, not an achievement, and reacting to it would turn a dead endpoint
    // into a retry storm.
    if (outcome.rfind("expired", 0) == 0)
        return;

    BringDecisionForward();
}

// Schedule the next decision as soon as the rate floor allows. The floor is
// measured from the last decision actually dispatched, so a burst of completions
// or deaths cannot outrun it.
void PlayerbotLongTermAI::BringDecisionForward()
{
    uint32 const now = getMSTime();
    uint32 const earliest = _lastDecisionMs + sPlayerbotAIConfig.llmDirectiveMinIntervalSeconds * 1000;
    uint32 const due = (now > earliest) ? now : earliest;

    // Only ever pull the decision earlier, never push it later.
    if (_nextDecisionMs == 0 || due < _nextDecisionMs)
        _nextDecisionMs = due;
}

std::string PlayerbotLongTermAI::SummariseDirectiveOutcome() const
{
    std::ostringstream out;

    if (_directiveApplyCount)
        out << "acted on it " << _directiveApplyCount << "x";
    else if (_directiveAligned)
        out << "you were already doing this";
    else
        out << "the engine never took it up";

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
