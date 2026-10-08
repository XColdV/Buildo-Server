#pragma once
#include <enet/enet.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "proto.h"
#include "world.h"

struct InvItem {
    uint16_t id = 0;
    uint8_t count = 0;
};

constexpr int kClothingSlots = 6;
constexpr uint8_t kMaxStack = 200;
constexpr size_t kMaxSlots = 64;  // different items a backpack holds

struct Player {
    ENetPeer* peer = nullptr;
    int netID = 0;
    int userID = 0;
    std::string name;
    std::string country = "us";
    std::string passwordHash;  // empty for names without a GrowID
    bool loggedIn = false;
    bool enteredGame = false;

    World* world = nullptr;
    float x = 0, y = 0;
    bool facingLeft = false;

    std::vector<InvItem> inventory;
    uint16_t clothes[kClothingSlots] = {};
    uint32_t skin = 0;
    int gems = 0;
    uint32_t lastChatMs = 0;
    uint32_t lastLockMsgMs = 0;
    std::string ip;
    std::vector<std::string> friends;
    std::vector<std::string> recentWorlds;
    int64_t mutedUntil = 0;
    std::vector<uint32_t> chatTimes;  // recent message times, for the spam limit
    std::string lastChat;
    std::string lastWhisperFrom;      // for /r

    int Count(uint16_t id) const;
    bool Add(uint16_t id, int count);  // false when it would not fit
    void Remove(uint16_t id, int count);
    bool Wearing(uint16_t id) const;
};

struct Options {
    int httpPort = 80;
    int enetPort = 17095;
    std::string host = "localhost";
    bool http = true;
    std::string gameDir;     // folder with Buildo.exe; searched for when empty
    std::string extraItems;  // extra_items.txt; searched for when empty
    std::string dataDir = "data";
};

extern Options g_opt;

void OnConnect(ENetPeer* peer);
void OnReceive(ENetPeer* peer, const uint8_t* data, size_t len);
void OnDisconnect(ENetPeer* peer);
void Tick();
void SaveEverything();
// A line typed into the server window (stop is handled by main).
void ConsoleCommand(const std::string& line);
