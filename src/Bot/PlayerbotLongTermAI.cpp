#include "PlayerbotLongTermAI.h"
#include "PlayerbotMgr.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

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
        PlayerbotsMgr::instance().RemovePlayerbotLongTermAI(bot->GetGUID());
}

void PlayerbotLongTermAI::UpdateAI(uint32 elapsed, bool minimal)
{
    // Early return if bot is in invalid state
    if (!bot || !bot->GetSession() || !bot->IsInWorld() || bot->IsBeingTeleported() ||
        bot->GetSession()->isLogingOut() || bot->IsDuringRemoveFromWorld())
        return;
    
    // This is naive for many reasons, the most critical being that it will put a significant
    // load on the server every 3 minutes.
    // TODO: After initial update, stagger the subsequent update then fall back on 3-minute intervals.
    uint32 now = getMSTime(); // AzerothCore helper: current time in ms
    uint32 intervalMs = 3 * 60 * 1000; // 3 minutes

    if (_timeLastUpdate == 0 || now - _timeLastUpdate >= intervalMs)
    {
        _timeLastUpdate = now;

        // Decide();
    }
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
