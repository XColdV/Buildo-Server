// What the game logic files share: the online players, loaded worlds and the
// helpers that talk to clients. Internal to the server, not a public API.
#pragma once
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "game.h"
#include "proto.h"
#include "world.h"

// Tank packet types (client switch 0x434418 / sender functions).
enum TankType : uint8_t {
    TANK_STATE = 0,
    TANK_CALL_FUNCTION = 1,
    TANK_TILE_CHANGE = 3,
    TANK_MAP_DATA = 4,
    TANK_TILE_UPDATE = 5,
    TANK_TILE_DAMAGE = 8,
    TANK_INVENTORY = 9,
    TANK_TREE_STATE = 12,
    TANK_REMOVE_ITEM = 13,
    TANK_ITEM_OBJECT = 14,
    TANK_LOCK = 15,
    TANK_ITEMS_DAT = 16,
    // client -> server only
    TANK_TILE_ACTIVATE = 7,
    TANK_ITEM_ACTIVATE = 10,
    TANK_OBJECT_PICKUP = 11,
};

using KeyValues = std::map<std::string, std::string>;

extern std::map<ENetPeer*, std::unique_ptr<Player>> g_players;
extern std::map<std::string, std::unique_ptr<World>> g_worlds;

uint32_t NowMs();
int Rand(int lo, int hi);
std::string Lower(std::string s);

void Send(ENetPeer* peer, uint32_t msgType, const void* data, size_t len);
void SendTank(ENetPeer* peer, TankPacket t, const std::vector<uint8_t>& ext = {});
void SendAction(ENetPeer* peer, const std::string& text);
// netID -1: a global call, run now (or after delayMs). Otherwise an avatar call.
void Call(ENetPeer* peer, const VariantList& v, int netID = -1, int delayMs = -1);
void Console(Player& p, const std::string& msg);
void Dialog(Player& p, const std::string& text);
void SendWorldTank(World* w, const TankPacket& t, const std::vector<uint8_t>& ext = {});
void SendInventory(Player& p);
void SendGems(Player& p);
VariantList ClothingCall(const Player& p, bool sound);

template <class F> void ForWorld(World* w, F f) {
    if (!w) return;
    for (auto& [peer, pl] : g_players)
        if (pl->world == w) f(*pl);
}

int PlayersIn(World* w);
void JoinWorld(Player& p, const std::string& rawName);
void LeaveWorld(Player& p, bool toMenu);
void DropItem(World& w, uint16_t item, int count, float x, float y);
void SendTileUpdate(World& w, int x, int y);
bool FindPlayer(const std::string& rawName, int& userID, std::string& name);
Player* OnlineByUserID(int userID);
Player* OnlineByName(const std::string& name);
void SavePlayer(Player& p);

// social.cpp: world list, friends, messages, moderation, chat limits.
void ShowWorldList(Player& p);
void RememberWorld(Player& p, const std::string& world);
bool IsWorldOwner(const Player& p, const World& w);
bool IsMod(const Player& p);
bool IsServerBanned(const std::string& name, const std::string& ip);
bool IsWorldBanned(const Player& p, const World& w);
bool ChatAllowed(Player& p, const std::string& msg);  // spam and mutes
void OnLoggedIn(Player& p);
void OnLoggedOut(Player& p);
bool SocialCommand(Player& p, const std::string& cmd, std::istringstream& args);
bool SocialDialog(Player& p, KeyValues& kv);
bool ServerConsoleCommand(const std::string& line);

// shop.cpp. CanTake: would these items fit in the backpack?
bool CanTake(const Player& p, const std::map<uint16_t, int>& items);
bool ShopCommand(Player& p, const std::string& cmd, std::istringstream& args);
bool ShopDialog(Player& p, KeyValues& kv);

// trade.cpp
bool TradeCommand(Player& p, const std::string& cmd, std::istringstream& args);
bool TradeDialog(Player& p, KeyValues& kv);
void CancelTrade(Player& p, const std::string& why);
