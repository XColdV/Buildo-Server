#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game.h"

struct Account {
    std::string name;  // as typed when the account was made
    int userID = 0;
    std::string passwordHash;  // empty: no GrowID, anyone may use the name
    std::vector<InvItem> inventory;
    uint16_t clothes[kClothingSlots] = {};
    uint32_t skin = 0;
    int gems = 0;
    std::vector<std::string> friends;       // names as shown
    std::vector<std::string> recentWorlds;  // newest first
    int64_t mutedUntil = 0;                 // unix seconds
};

bool LoadAccount(const std::string& dataDir, const std::string& name, Account& out);
bool SaveAccount(const std::string& dataDir, const std::string& name, const Account& acc);
std::string HashPassword(const std::string& name, const std::string& password);
int NextUserID(const std::string& dataDir);
