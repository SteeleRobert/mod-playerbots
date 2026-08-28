#ifndef PLAYERBOTS_PLAYERBOTLONGTERMAI_H
#define PLAYERBOTS_PLAYERBOTLONGTERMAI_H

#include "PlayerbotAI.h"
#include "FunctionTool.h"
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

class PlayerbotLongTermAI
{
public:
    PlayerbotLongTermAI();
    PlayerbotLongTermAI(Player* bot);
    virtual ~PlayerbotLongTermAI();

    void UpdateAI(uint32 elapsed, bool minimal = false);
    void Decide();

protected:
    Player* bot;
    FunctionToolRegistry functionToolRegistry;

private:
    uint32 _timeLastUpdate;
};

#endif
