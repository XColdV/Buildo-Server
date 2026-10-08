// Builds the items.dat the client downloads from game/item_definitions.txt.
// Record layout is the client's item deserializer (0x43acc0); the file must
// be dense because the client indexes the item array by ID.
#include "items.h"
#include "log.h"
#include "proto.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

namespace {

std::vector<ItemDef> g_items;
std::vector<ShopEntry> g_shop;
std::map<uint32_t, uint16_t> g_splices;  // (low seed << 16 | high seed) -> result seed
std::vector<uint8_t> g_dat;
uint32_t g_hash = 0;

// Material values the client switches on (spec: 0x43e48e, 0x43e432, 0x4412de...).
const std::map<std::string, uint8_t> kMaterials = {
    {"TILE_MATERIAL_FIST", 0},       {"TILE_MATERIAL_WRENCH", 1},    {"TILE_MATERIAL_USER_DOOR", 2},
    {"TILE_MATERIAL_LOCK", 3},       {"TILE_MATERIAL_SIGN", 4},      {"TILE_MATERIAL_BOOMBOX", 6},
    {"TILE_MATERIAL_SOUND_BLOCK", 5},
    {"TILE_MATERIAL_DOOR", 7},       {"TILE_MATERIAL_ROCK", 8},      {"TILE_MATERIAL_WOOD", 8},
    {"TILE_MATERIAL_BEDROCK", 9},    {"TILE_MATERIAL_LAVA", 10},     {"TILE_MATERIAL_DIRT", 11},
    {"TILE_MATERIAL_BACKGROUND", 12}, {"TILE_MATERIAL_SEED", 13},    {"TILE_MATERIAL_CLOTHES", 14},
};
constexpr uint8_t MAT_FIST = 0, MAT_WRENCH = 1, MAT_USER_DOOR = 2, MAT_LOCK = 3, MAT_SIGN = 4, MAT_DOOR = 7,
                  MAT_BEDROCK = 9, MAT_BACKGROUND = 12, MAT_SEED = 13, MAT_CLOTHES = 14, MAT_GEMS = 15,
                  MAT_DEFAULT = 11;

const std::map<std::string, uint8_t> kStorage = {
    {"TILE_STORAGE_SINGLE_FRAME_IN_TILESHEET", 1},
    {"TILE_STORAGE_SMART_EDGE", 2},
    {"TILE_STORAGE_SMART_EDGE_HORIZONTAL", 3},  // 4x1 sheet, picks by left/right neighbours
};

const std::map<std::string, uint8_t> kBodyParts = {
    {"HAT", 0}, {"SHIRT", 1}, {"PANTS", 2}, {"SHOES", 3}, {"FACEITEM", 4}, {"HAND", 5},
};

uint32_t Rgba(int r, int g, int b, int a) {
    return static_cast<uint32_t>(a & 0xff) | (static_cast<uint32_t>(r & 0xff) << 8) |
           (static_cast<uint32_t>(g & 0xff) << 16) | (static_cast<uint32_t>(b & 0xff) << 24);
}

uint32_t ParseColor(const std::string& s) {
    int c[4] = {255, 255, 255, 255};
    sscanf(s.c_str(), "%d,%d,%d,%d", &c[0], &c[1], &c[2], &c[3]);
    return Rgba(c[0], c[1], c[2], c[3]);
}

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '|') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

ItemDef& Slot(uint32_t id) {
    if (id >= g_items.size()) g_items.resize(id + 1);
    g_items[id].id = static_cast<uint16_t>(id);
    return g_items[id];
}

uint32_t ClientHash(const uint8_t* p, size_t len) {
    uint32_t h = 0x55555555;
    for (size_t i = 0; i < len; i++) h = ((h << 5) | (h >> 27)) + p[i];
    return h;
}

// A tree has to exist for every block a player can break, because the client
// reads a seed's fruit as (seed id - 1).
bool WantsSeed(const ItemDef& it) {
    if (!it.defined || it.isSeed || it.noSeed || it.name == "Unused") return false;
    switch (it.material) {
    case MAT_FIST: case MAT_WRENCH: case MAT_DOOR: case MAT_BEDROCK: case MAT_CLOTHES: case MAT_GEMS:
        return false;
    case MAT_LOCK:
        return false;
    default:
        return true;
    }
}

void Serialize() {
    Writer w;
    w.u16(1);
    w.u32(static_cast<uint32_t>(g_items.size()));
    for (const ItemDef& it : g_items) {
        w.u32(it.id);
        w.u8(it.material);
        w.u8(it.visualEffect);
        w.str16(it.name);
        w.str16(it.texture);
        w.u32(0);  // texture hash: 0 keeps the client's downloader off
        w.u8(0);   // layer
        w.u32(it.tint);
        w.u8(it.texX);
        w.u8(it.texY);
        w.u8(it.storage);
        w.u8(0);
        w.u8(it.collision);
        w.u8(it.hp);
        w.u32(it.healSeconds);
        w.u8(it.bodyPart);
        w.u16(it.rarity);
        w.u8(it.maxCanHold);
        w.str16(it.sound);  // extra (audio) file, path as-is
        w.u32(0);     // extra file hash
        w.u32(400);   // animation interval ms
        w.u8(it.seedBase);
        w.u8(it.seedOverlay);
        w.u8(it.treeBase);
        w.u8(it.treeLeaves);
        w.u32(it.seedBgColor);
        w.u32(it.seedFgColor);
        w.u16(it.seed1);
        w.u16(it.seed2);
        w.u32(it.growSeconds);
    }
    g_dat = std::move(w.buf);
    g_hash = ClientHash(g_dat.data(), g_dat.size());
    if (g_hash == 0) g_hash = 1;  // 0 would match a missing cache file and crash the client
}

}  // namespace

namespace {

bool ReadFile(const std::string& path, std::string& text) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    return true;
}

void ParseDefinitions(const std::string& text, const std::string& fileName) {
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    int last = -1;  // index, not a pointer: Slot() may grow the vector
    while (std::getline(in, line)) {
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.rfind("//", 0) == 0) continue;
        auto f = Split(line);
        const std::string& cmd = f[0];
        auto num = [&](size_t i) { return i < f.size() ? atoi(f[i].c_str()) : 0; };
        auto field = [&](size_t i) { return i < f.size() ? f[i] : std::string(); };
        auto unknown = [&](const std::string& what) {
            LogError("%s line %d: unknown value %s", fileName.c_str(), lineNo, what.c_str());
        };
        auto lookup = [&](const std::map<std::string, uint8_t>& table, const std::string& key, uint8_t fallback) {
            auto it = table.find(key);
            if (it != table.end()) return it->second;
            unknown(key);
            return fallback;
        };

        if (cmd == "add_tile" && f.size() >= 13) {
            ItemDef& it = Slot(num(1));
            it.defined = true;
            it.name = field(2);
            it.material = lookup(kMaterials, field(3), MAT_DEFAULT);
            it.storage = lookup(kStorage, field(5), 1);
            it.texX = static_cast<uint8_t>(num(6));
            it.texY = static_cast<uint8_t>(num(7));
            it.texture = field(8);
            it.collision = field(11) == "TILE_COLLISION_SOLID" ? 1 : 0;
            it.hp = static_cast<uint8_t>(f.size() > 12 && !f[12].empty() ? num(12) : 4);
            it.healSeconds = static_cast<uint32_t>(f.size() > 13 && !f[13].empty() ? num(13) : 8);
            last = it.id;
        } else if (cmd == "add_clothes" && f.size() >= 11) {
            ItemDef& it = Slot(num(1));
            it.defined = true;
            it.name = field(2);
            it.material = MAT_CLOTHES;
            it.storage = lookup(kStorage, field(5), 1);
            it.texX = static_cast<uint8_t>(num(6));
            it.texY = static_cast<uint8_t>(num(7));
            it.texture = field(8);
            it.collision = 0;
            it.hp = 1;
            it.bodyPart = lookup(kBodyParts, field(10), 0);
            last = it.id;
        } else if (cmd == "setup_seed" && f.size() >= 2) {
            ItemDef& it = Slot(num(1));
            it.isSeed = true;
            for (size_t i = 2; i + 1 < f.size(); i += 2) {
                const std::string& k = f[i];
                if (k == "seed1") it.seed1 = static_cast<uint16_t>(num(i + 1));
                else if (k == "seed2") it.seed2 = static_cast<uint16_t>(num(i + 1));
                else if (k == "seconds_to_bloom") it.growSeconds = static_cast<uint32_t>(num(i + 1));
                else if (k == "max_fruit") it.maxFruit = static_cast<uint8_t>(num(i + 1));
                else if (k == "bg_color") it.seedBgColor = ParseColor(f[i + 1]);
                else if (k == "fg_color") it.seedFgColor = ParseColor(f[i + 1]);
            }
            last = -1;
        } else if (cmd == "set_color" && f.size() >= 5) {
            if (last >= 0) g_items[last].tint = Rgba(num(1), num(2), num(3), num(4));
        } else if (cmd == "set_max_can_hold" && f.size() >= 3) {
            Slot(num(1)).maxCanHold = static_cast<uint8_t>(num(2));
        } else if (cmd == "set_no_seed" && f.size() >= 2) {
            Slot(num(1)).noSeed = true;
        } else if (cmd == "set_sound" && f.size() >= 3) {
            Slot(num(1)).sound = field(2);
        } else if (cmd == "add_shop" && f.size() >= 4) {
            // add_shop|item id|count|price|
            ShopEntry e;
            e.item = static_cast<uint16_t>(num(1));
            e.count = std::max(1, num(2));
            e.price = num(3);
            g_shop.push_back(e);
        } else if (cmd == "add_shop_pack" && f.size() >= 5) {
            // add_shop_pack|name|price|picks|id,id,id|
            ShopEntry e;
            e.name = field(1);
            e.price = num(2);
            e.count = std::max(1, num(3));
            std::istringstream ids(field(4));
            std::string id;
            while (std::getline(ids, id, ',')) e.choices.push_back(static_cast<uint16_t>(atoi(id.c_str())));
            if (!e.choices.empty()) g_shop.push_back(e);
        } else if (cmd == "set_lock_size" && f.size() >= 3) {
            // Server-side only: tiles an area lock claims, 0 = the whole world.
            Slot(num(1)).lockSize = num(2);
        } else {
            LogError("%s line %d: skipped: %s", fileName.c_str(), lineNo, line.c_str());
        }
    }
}

}  // namespace

bool LoadItems(const std::string& definitionsPath, const std::string& extraPath) {
    std::string text;
    if (!ReadFile(definitionsPath, text)) {
        LogError("cannot open %s", definitionsPath.c_str());
        return false;
    }
    g_items.clear();
    g_shop.clear();
    g_splices.clear();
    ParseDefinitions(text, "item_definitions.txt");
    std::string extra;
    if (!extraPath.empty() && ReadFile(extraPath, extra)) ParseDefinitions(extra, "extra_items.txt");
    else LogError("no extra_items.txt, so no Small/Big/Huge/World Locks");

    // Gems: the client adds pickups of item 112 to its bux counter and draws
    // material 15 drops from tiles_bux.rttex.
    {
        ItemDef& g = Slot(ITEM_GEMS);
        g.defined = true;
        g.name = "Gems";
        g.material = MAT_GEMS;
        g.texture = "tiles_bux.rttex";
        g.storage = 1;
        g.collision = 0;
        g.hp = 1;
    }

    // Seeds, and filler records so the array has no holes.
    if (WantsSeed(g_items.back())) Slot(static_cast<uint32_t>(g_items.size()));
    for (size_t id = 0; id < g_items.size(); id++) {
        ItemDef& it = Slot(static_cast<uint32_t>(id));
        bool parentWantsSeed = (id % 2 == 1) && WantsSeed(g_items[id - 1]);
        if (id > 0 && (it.isSeed || (parentWantsSeed && !it.defined))) {
            const ItemDef& fruit = g_items[id - 1];
            it.defined = true;
            it.isSeed = true;
            it.name = fruit.name + " Seed";
            it.material = MAT_SEED;
            it.texture = "seed.rttex";
            it.storage = 1;
            it.collision = 0;
            it.hp = 1;
            it.healSeconds = 4;
            if (it.growSeconds == 0) it.growSeconds = 30 + 10u * fruit.hp;
            if (it.maxFruit == 0) it.maxFruit = 3;
            // Colours from setup_seed when given, otherwise a stable per-item pick.
            uint32_t mix = ClientHash(reinterpret_cast<const uint8_t*>(fruit.name.data()), fruit.name.size());
            if (it.seedBgColor == 0 || (it.seedBgColor & 0xff) == 0)
                it.seedBgColor = Rgba(60 + mix % 160, 60 + (mix >> 8) % 160, 60 + (mix >> 16) % 160, 255);
            if (it.seedFgColor == 0 || (it.seedFgColor & 0xff) == 0)
                it.seedFgColor = Rgba(90 + (mix >> 4) % 160, 90 + (mix >> 12) % 160, 90 + (mix >> 20) % 160, 255);
            it.seedBase = static_cast<uint8_t>(mix % 4);
            it.seedOverlay = static_cast<uint8_t>((mix >> 3) % 4);
            it.treeBase = static_cast<uint8_t>((mix >> 6) % 4);
            it.treeLeaves = static_cast<uint8_t>((mix >> 9) % 4);
        } else if (!it.defined) {
            it.name = "Unused";
            it.material = MAT_DEFAULT;
            it.collision = 0;
            it.hp = 1;
        }
    }

    for (const ItemDef& it : g_items) {
        if (!it.isSeed || !it.seed1 || !it.seed2) continue;
        uint16_t a = std::min(it.seed1, it.seed2), b = std::max(it.seed1, it.seed2);
        uint32_t key = (static_cast<uint32_t>(a) << 16) | b;
        if (g_splices.count(key)) LogError("two seeds splice from %u + %u; keeping %u", a, b, g_splices[key]);
        else g_splices[key] = it.id;
    }
    // Drop shop lines that point at items that don't exist.
    g_shop.erase(std::remove_if(g_shop.begin(), g_shop.end(), [](const ShopEntry& e) {
        if (e.item) return GetItem(e.item) == nullptr;
        for (uint16_t c : e.choices)
            if (!GetItem(c)) return true;
        return false;
    }), g_shop.end());

    Serialize();
    int seeds = 0, real = 0;
    for (auto& it : g_items) {
        if (it.isSeed) seeds++;
        else if (it.defined && it.name != "Unused") real++;
    }
    Log("items.dat: %zu records (%d items, %d seeds), %zu bytes, hash %u", g_items.size(), real, seeds,
        g_dat.size(), g_hash);
    return true;
}

const ItemDef* GetItem(uint32_t id) {
    if (id >= g_items.size() || !g_items[id].defined) return nullptr;
    return &g_items[id];
}
const std::vector<ItemDef>& AllItems() { return g_items; }
const std::vector<uint8_t>& ItemsDat() { return g_dat; }
const std::vector<ShopEntry>& ShopEntries() { return g_shop; }

uint16_t SpliceResult(uint16_t seedA, uint16_t seedB) {
    uint16_t a = std::min(seedA, seedB), b = std::max(seedA, seedB);
    auto it = g_splices.find((static_cast<uint32_t>(a) << 16) | b);
    return it == g_splices.end() ? 0 : it->second;
}
uint32_t ItemsDatHash() { return g_hash; }

ExtraType ExtraTypeFor(uint8_t material) {
    switch (material) {
    case MAT_USER_DOOR: case MAT_DOOR: return EXTRA_DOOR;
    case MAT_SIGN: return EXTRA_SIGN;
    case MAT_LOCK: return EXTRA_LOCK;
    case MAT_SEED: return EXTRA_TREE;
    default: return EXTRA_NONE;
    }
}
bool IsBackground(const ItemDef& i) { return i.material == MAT_BACKGROUND; }
bool IsClothes(const ItemDef& i) { return i.material == MAT_CLOTHES; }
bool IsSeedItem(const ItemDef& i) { return i.material == MAT_SEED; }
bool IsFist(const ItemDef& i) { return i.material == MAT_FIST; }
bool IsWrench(const ItemDef& i) { return i.material == MAT_WRENCH; }
bool IsDoor(const ItemDef& i) { return i.material == MAT_DOOR || i.material == MAT_USER_DOOR; }
bool IsMainDoor(const ItemDef& i) { return i.material == MAT_DOOR; }
bool IsLock(const ItemDef& i) { return i.material == MAT_LOCK; }
bool IsSign(const ItemDef& i) { return i.material == MAT_SIGN; }
bool IsUnbreakable(const ItemDef& i) { return i.material == MAT_DOOR || i.material == MAT_BEDROCK; }
