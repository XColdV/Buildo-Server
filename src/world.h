#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct Tile {
    uint16_t fg = 0;
    uint16_t bg = 0;
    std::string label;   // sign text / door destination, when the tile has one
    // Locks
    int32_t owner = 0;         // userID of the lock's owner (lock tiles only)
    std::string ownerName;
    std::vector<std::pair<int32_t, std::string>> access;  // userID, name
    bool ignoreEmpty = false;  // area locks: don't count empty tiles
    uint16_t lockParent = 0;   // index of the area lock covering this tile, 0 = none
    uint8_t damage = 0;  // hits taken; not saved
    uint32_t lastHitMs = 0;
    int64_t plantedAt = 0;  // unix seconds, seeds only
    uint8_t fruit = 0;      // seeds only
};

struct DroppedItem {
    uint16_t item = 0;
    float x = 0, y = 0;
    uint8_t count = 0;
    uint8_t flags = 0;
    uint32_t id = 0;
};

struct World {
    std::string name;
    int width = 100;
    int height = 60;
    std::vector<Tile> tiles;
    std::vector<DroppedItem> drops;
    uint32_t lastObjectID = 0;  // the client numbers new drops from this, keep in step
    int worldLock = -1;         // tile index of the lock that covers the whole world
    std::vector<std::pair<int32_t, std::string>> bans;  // userID, name; set by the owner
    bool dirty = false;

    Tile* At(int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height) return nullptr;
        return &tiles[static_cast<size_t>(y) * width + x];
    }
    // Tile of the white main door, where players spawn.
    bool FindDoor(int& x, int& y) const;
    int Index(int x, int y) const { return y * width + x; }
};

void GenerateWorld(World& w);
// Tiles an area lock at lockIndex claims, nearest first. It grows through
// 4-connected tiles and stops at other locks and their areas.
std::vector<uint16_t> ComputeLockArea(const World& w, int lockIndex, int size, bool ignoreEmpty);
// Sets worldLock from the tiles, after loading.
void FindWorldLock(World& w);
bool LoadWorld(const std::string& dir, World& w);
bool SaveWorld(const std::string& dir, const World& w);
// Upper-case A-Z/0-9, at most 24 chars; empty when invalid.
std::string NormalizeWorldName(const std::string& raw);

// Map data as the client reads it (World::Serialize net mode, 0x43f450).
std::vector<uint8_t> SerializeWorld(const World& w);
// One tile in net mode (0x43e850), used by map data and tile updates.
void SerializeTile(class Writer& out, const Tile& t);
