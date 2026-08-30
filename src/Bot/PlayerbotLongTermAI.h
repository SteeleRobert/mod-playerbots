#ifndef PLAYERBOTS_PLAYERBOTLONGTERMAI_H
#define PLAYERBOTS_PLAYERBOTLONGTERMAI_H

#include "PlayerbotAI.h"
#include "FunctionTool.h"
#include "LongTerm/LlmClient.h"
#include "LongTerm/LlmDirective.h"
#include "LongTerm/LlmVendor.h"
#include <nlohmann/json.hpp>
using json = nlohmann::json;

class Player;

class FunctionToolRegistry
{
public:
    void AddTool(std::string name, std::unique_ptr<FunctionTool> handler);
    json Handle(std::string name, json params);

private:
    std::unordered_map<std::string, std::unique_ptr<FunctionTool>> tools;
};

/*
 * The slow half of a two-speed brain.
 *
 * PlayerbotAI ticks every ~100 ms and owns every second-to-second decision. This
 * class runs on the same update hook but acts roughly once every five minutes, and
 * all it ever produces is a single LlmDirective: an *intent* ("quest", "grind",
 * "travel to Westfall", "go turn things in", "go to a town"). It never moves the
 * bot, never picks a target and never touches the action queue.
 *
 * The directive is consumed by the classical New RPG state machine, which gets
 * first refusal on the status roll (NewRpgBaseAction::RandomChangeStatus). If the
 * directive cannot be honoured - no quest with POI data, no flight path, endpoint
 * down, reply unparseable - the classical roll runs exactly as it always has. The
 * LLM is never in the critical path, and it is never allowed to freeze a bot.
 *
 * Everything here is inert unless AiPlayerbot.LlmDirective.Enabled is set AND the
 * individual bot is opted in, so the A/B baseline keeps its original code path.
 */
class PlayerbotLongTermAI
{
public:
    PlayerbotLongTermAI();
    PlayerbotLongTermAI(Player* bot);
    virtual ~PlayerbotLongTermAI();

    void UpdateAI(uint32 elapsed, bool minimal = false);
    void Decide();

    // --- consumed by the classical engine (New RPG) ---------------------------

    // True when the feature is on and this bot is one of the opted-in ones.
    bool IsDirectiveLayerActive();

    // True when this bot must play honestly: no teleporting in place of
    // walking, no free repair/heal/money/restock. Asked by the classical
    // subsystems that own those cheats, so it is static and null-safe.
    static bool IsHonestBot(Player* bot);

    // RPG statuses to try, in order, before falling back to the normal weighted
    // roll. Empty whenever there is nothing to say, which is the common case.
    std::vector<NewRpgStatus> GetPreferredRpgStatuses();

    LlmDirective const& GetDirective() const { return _directive; }

    // Zone the current directive wants the bot in, or 0.
    uint32 GetDirectiveZoneId() const;

    // "turnin" asks the quest picker to prefer quests that are already complete.
    bool PrefersCompletedQuests() const;

    // Quest the current directive named, or 0 to let the engine pick.
    uint32 GetDirectiveQuestId() const;

    // The engine took the directive up; recorded so the next prompt can say
    // whether the intent ever actually reached the world.
    void NoteDirectiveApplied(NewRpgStatus status);

    // GO_CAMP hands an arrived vendor directive to this separately prompted
    // agent. The New RPG vendor action owns movement and trade execution.
    bool BeginVendorHandoff();
    bool IsVendorHandoffActive() const;
    bool GetVendorPlan(LlmVendorPlan& out) const;
    void CompleteVendorHandoff(std::string const& outcome);

protected:
    Player* bot;
    FunctionToolRegistry functionToolRegistry;

private:
    bool ComputeOptIn() const;
    void RequestDecision(uint32 now);
    void ConsumeReply(LlmReply const& reply);
    void ConsumeVendorReply(LlmReply const& reply);
    void CheckDirectiveCompletion();
    void BringDecisionForward();
    bool BotHoldsIncompleteQuest(uint32 questId) const;
    std::string SummariseDirectiveOutcome() const;
    void RetireDirective(std::string const& outcome);

    uint32 _timeLastUpdate;

    // -1 not yet computed, 0 no, 1 yes. Recomputed only on a config reload, which
    // bumps _optInGeneration.
    int8 _optIn{-1};

    uint32 _nextDecisionMs{0};
    // When the last decision was dispatched, so the minimum-interval floor
    // applies to completion-triggered decisions too, not just scheduled ones.
    uint32 _lastDecisionMs{0};
    bool _pending{false};
    std::string _pendingPrompt;
    std::vector<LlmZoneChoice> _pendingZones;
    std::vector<uint32> _pendingQuests;

    bool _vendorHandoff{false};
    bool _vendorPending{false};
    bool _vendorPlanReady{false};
    bool _vendorRepairOffered{false};
    std::string _vendorPrompt;
    std::vector<LlmVendorItem> _vendorItems;
    LlmVendorPlan _vendorPlan;

    LlmDirective _directive;
    uint32 _directiveApplyCount{0};
    // The bot was already in a status the directive asked for, so the engine had
    // no re-roll to make. That is the directive being honoured, not ignored - the
    // apply counter alone reported those as "never took it up".
    bool _directiveAligned{false};

    // State snapshot taken when a directive is issued, so its outcome can be
    // described in concrete terms rather than "it ran".
    uint32 _atIssueQuestRewarded{0};
    uint32 _atIssueQuestAccepted{0};
    uint32 _atIssueLevel{0};
    uint32 _atIssueZoneId{0};

    uint32 _deathsSinceLastDecision{0};
    bool _wasDead{false};

    // Last values seen by the dashboard event feed. Separate from the
    // directive snapshot, because events must fire between decisions too.
    uint32 _telemetryLevel{0};
    uint32 _telemetryQuestRewarded{0};
    bool _telemetryPrimed{false};
    uint32 _lastPositionSampleMs{0};
};

#endif
