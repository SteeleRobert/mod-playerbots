/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_LOOTOBJECTSTACK_H
#define PLAYERBOTS_LOOTOBJECTSTACK_H

#include "ObjectGuid.h"

#include <ctime>
#include <unordered_map>

class AiObjectContext;
class Player;
class WorldObject;

struct ItemTemplate;

class LootStrategy
{
public:
    LootStrategy() {}
    virtual ~LootStrategy(){};
    virtual bool CanLoot(ItemTemplate const* proto, AiObjectContext* context) = 0;
    virtual std::string const GetName() = 0;
};

class LootObject
{
public:
    LootObject() : skillId(0), reqSkillValue(0), reqItem(0) {}
    LootObject(Player* bot, ObjectGuid guid);
    LootObject(LootObject const& other);
    LootObject& operator=(LootObject const& other) = default;

    bool IsEmpty() { return !guid; }
    bool IsLootPossible(Player* bot);
    void Refresh(Player* bot, ObjectGuid guid);
    WorldObject* GetWorldObject(Player* bot);
    ObjectGuid guid;

    uint32 skillId;
    uint32 reqSkillValue;
    uint32 reqItem;

private:
    static bool IsNeededForQuest(Player* bot, uint32 itemId);
};

class LootTarget
{
public:
    LootTarget(ObjectGuid guid);
    LootTarget(LootTarget const& other);

public:
    LootTarget& operator=(LootTarget const& other);
    bool operator<(LootTarget const& other) const;

public:
    ObjectGuid guid;
    time_t asOfTime;
};

class LootTargetList : public std::set<LootTarget>
{
public:
    void shrink(time_t fromTime);
};

class LootObjectStack
{
public:
    LootObjectStack(Player* bot) : bot(bot) {}

    bool Add(ObjectGuid guid);
    void Remove(ObjectGuid guid);
    void Clear();
    bool CanLoot(float maxDistance);
    LootObject GetLoot(float maxDistance = 0);

    // A loot attempt that did not succeed. After MAX_LOOT_ATTEMPTS of these the
    // object is put aside for LOOT_BLACKLIST_SECONDS and Add() refuses it, which
    // is the only thing that breaks the loop: the scanner re-adds any node still
    // in range, so removing it alone lasts exactly one tick.
    // Called on every attempt, whatever the action reported. OpenLootAction returns
    // OK the moment a cast is started, so "did it succeed" carries no information:
    // a bot with a full bag casts, is refused by the server, and is told OK. What
    // does carry information is that the same object is still being attempted.
    void NoteAttempt(ObjectGuid guid);

private:
    LootObject GetNearest(float maxDistance = 0);
    bool IsSetAside(ObjectGuid guid);

    // Five is enough to ride out a transient miss (global cooldown, a cast
    // clipped by movement) without leaving the bot pinned to a node it can
    // never take. The wait is short because the usual cause - a full bag - is
    // itself temporary, and a bot that has since sold should try again.
    // Time, not tries: a gather cast runs several seconds and a fixed count of
    // attempts would be burned through before it ever had a chance to land.
    //
    // A minute. This is a backstop, not the fix - the real cause was StoreLootAction
    // declining items above 80% bag space - so it is set well clear of anything
    // legitimate rather than tuned tight. A node that is actually taken despawns, so
    // a working interaction is never seen twice; only one that refuses to die can
    // reach sixty seconds, and by then the bot has plainly been standing there.
    static constexpr uint32 STUCK_ON_OBJECT_SECONDS = 60;
    // A gap this long means the bot left and came back, so the clock restarts
    // rather than holding it responsible for an older visit.
    static constexpr uint32 ATTEMPT_GAP_SECONDS = 10;
    static constexpr uint32 LOOT_BLACKLIST_SECONDS = 15 * 60;
    // Hard ceiling on the failure table. A first failure is common - a clipped
    // cast, a global cooldown - so without a cap this grows an entry for nearly
    // every corpse and node the bot ever touches and never gives one back. At
    // 3000 bots that is measured in gigabytes; at this cap it is a few KB each.
    static constexpr size_t MAX_TRACKED_FAILURES = 64;

    struct LootFailure
    {
        time_t firstAttempt{0};
        time_t lastAttempt{0};
        time_t expiresAt{0};
        bool setAside{false};
    };

    void PruneFailures(time_t now);

    Player* bot;
    LootTargetList availableLoot;
    std::unordered_map<uint64, LootFailure> lootFailures;
};

#endif
