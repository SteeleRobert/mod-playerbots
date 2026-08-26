#ifndef PLAYERBOTS_FUNCTIONTOOL_H
#define PLAYERBOTS_FUNCTIONTOOL_H

#include "Define.h"
#include <nlohmann/json.hpp>
using json = nlohmann::json;

enum LongTermStrategy
{
    AFK,
    QUESTING,
    EXPLORATION,
    COMBAT,
    GATHERING,
    CRAFTING
};

std::string ToString(LongTermStrategy strategy);

// Returns a random integer in the inclusive range [min, max].
int32 GetRandomNumber(int32 min, int32 max);

// Returns a randomly chosen LongTermStrategy.
LongTermStrategy GetRandomStrategy();

class FunctionTool {
public:
    virtual ~FunctionTool() = default;
    virtual std::string getName() const = 0;
    virtual std::string getDesc() const = 0;
    virtual json Handle(const json& params) = 0;
};

class GodFunctionTool : public FunctionTool
{
public:
    std::string getName() const override;
    std::string getDesc() const override;
    json Handle(const json& params) override;
};

#endif
