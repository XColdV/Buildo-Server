#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum ItemID : uint16_t {
    ITEM_BLANK = 0,
    ITEM_DIRT = 2,
    ITEM_LAVA = 4,
    ITEM_DOOR = 6,
    ITEM_BEDROCK = 8,
    ITEM_ROCK = 10,
    ITEM_CAVE_WALL = 14,
    ITEM_FIST = 18,
    ITEM_WRENCH = 32,
    ITEM_GEMS = 112,  // the client adds pickups of this id to its bux counter
};

// Tile extra types (TileExtra 0x43f070).
enum ExtraType : uint8_t {
    EXTRA_NONE = 0,
    EXTRA_DOOR = 1,
    EXTRA_SIGN = 2,
    EXTRA_LOCK = 3,
    EXTRA_TREE = 4,
};

struct ItemDef {
    uint16_t id = 0;
    std::string name;
    uint8_t material = 0;    // item+0x04
    uint8_t visualEffect = 0;
    std::string texture;
    uint32_t tint = 0xFFFFFFFF;  // set_color
    uint8_t texX = 0, texY = 0;
    uint8_t storage = 0;
    uint8_t collision = 0;
    uint8_t hp = 0;          // hits to break, item+0x60
    uint32_t healSeconds = 0;
    uint8_t bodyPart = 0;    // clothing slot, item+0x68
    uint16_t rarity = 0;
    uint8_t maxCanHold = 200;  // 0 = never used up
    int lockSize = -1;         // locks: tiles claimed, 0 = whole world, -1 = not set
    bool defined = false;
    // Seeds
    bool isSeed = false;
    uint16_t seed1 = 0, seed2 = 0;
    uint32_t growSeconds = 0;
    uint8_t maxFruit = 0;
    uint32_t seedBgColor = 0, seedFgColor = 0;
    uint8_t seedBase = 0, seedOverlay = 0, treeBase = 0, treeLeaves = 0;
};

// The game's item_definitions.txt, then the server's own additions (optional).
bool LoadItems(const std::string& definitionsPath, const std::string& extraPath);
const ItemDef* GetItem(uint32_t id);
const std::vector<ItemDef>& AllItems();
const std::vector<uint8_t>& ItemsDat();
uint32_t ItemsDatHash();
ExtraType ExtraTypeFor(uint8_t material);
bool IsBackground(const ItemDef& item);
bool IsClothes(const ItemDef& item);
bool IsSeedItem(const ItemDef& item);
bool IsFist(const ItemDef& item);
bool IsWrench(const ItemDef& item);
bool IsDoor(const ItemDef& item);  // main door or user door
bool IsMainDoor(const ItemDef& item);
bool IsLock(const ItemDef& item);
bool IsSign(const ItemDef& item);
bool IsUnbreakable(const ItemDef& item);
