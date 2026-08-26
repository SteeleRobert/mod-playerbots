#include "FunctionTool.h"
#include "Util.h"
#include "Define.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

std::string ToString(LongTermStrategy strategy)
{
    switch (strategy)
    {
        case LongTermStrategy::AFK:         return "AFK";
        case LongTermStrategy::QUESTING:    return "QUESTING";
        case LongTermStrategy::EXPLORATION: return "EXPLORATION";
        case LongTermStrategy::COMBAT:      return "COMBAT";
        case LongTermStrategy::GATHERING:   return "GATHERING";
        case LongTermStrategy::CRAFTING:    return "CRAFTING";
        default:                            return "UNKNOWN";
    }
}

int32 GetRandomNumber(int32 min, int32 max)
{
    return static_cast<int32>(urand(static_cast<uint32>(min), static_cast<uint32>(max)));
}

LongTermStrategy GetRandomStrategy()
{
    return static_cast<LongTermStrategy>(GetRandomNumber(LongTermStrategy::AFK, LongTermStrategy::CRAFTING));
}

std::string GodFunctionTool::getName() const {
    return "GodFunctionTool";
}

std::string GodFunctionTool::getDesc() const {
    return "God decides your fate.";
}

json GodFunctionTool::Handle(const json& params)
{
    LongTermStrategy strategy = GetRandomStrategy();
    return {{
        {"name", getName()},
        {"params", params},
        {"result", ToString(strategy)}
    }};
}
