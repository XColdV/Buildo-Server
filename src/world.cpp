#include "world.h"
#include "items.h"
#include "proto.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <queue>
#include <random>

bool World::FindDoor(int& x, int& y) const {
    for (int i = 0; i < width * height; i++) {
        if (tiles[i].fg == ITEM_DOOR) {
            x = i % width;
            y = i / width;
            return true;
        }
    }
    return false;
}

std::string NormalizeWorldName(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(toupper(static_cast<unsigned char>(c)));
        else return {};
    }
    if (out.empty() || out.size() > 24) return {};
    return out;
}

// Same shape as the classic generator: sky on top, a dirt layer with cave wall
// behind it, rock and lava scattered underground, bedrock at the bottom and the
// main door on the surface with bedrock under it.
void GenerateWorld(World& w) {
    w.tiles.assign(static_cast<size_t>(w.width) * w.height, Tile{});
    std::mt19937 rng(std::random_device{}());
    const int surface = w.height * 2 / 5;   // 24 on a 60-high world
    const int bedrockFrom = w.height - 6;

    for (int y = surface; y < w.height; y++) {
        for (int x = 0; x < w.width; x++) {
            Tile& t = *w.At(x, y);
            t.bg = ITEM_CAVE_WALL;
            if (y >= bedrockFrom) {
                t.fg = ITEM_BEDROCK;
            } else if (y > surface + 5 && rng() % 100 < 4) {
                t.fg = ITEM_ROCK;
            } else if (y > surface + 15 && rng() % 100 < 3) {
                t.fg = ITEM_LAVA;
            } else {
                t.fg = ITEM_DIRT;
            }
        }
    }
    int doorX = 2 + static_cast<int>(rng() % (w.width - 4));
    w.At(doorX, surface - 1)->fg = ITEM_DOOR;
    w.At(doorX, surface - 1)->label = "EXIT";
    w.At(doorX, surface)->fg = ITEM_BEDROCK;
    w.dirty = true;
}

std::vector<uint16_t> ComputeLockArea(const World& w, int lockIndex, int size, bool ignoreEmpty) {
    // Best-first over reachable tiles by straight-line distance from the lock,
    // so the area comes out round instead of diamond shaped.
    const int lx = lockIndex % w.width, ly = lockIndex / w.width;
    auto dist = [&](int i) {
        int dx = i % w.width - lx, dy = i / w.width - ly;
        return dx * dx + dy * dy;
    };
    using Entry = std::pair<int, int>;  // distance, index
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    std::vector<char> seen(w.tiles.size(), 0);
    std::vector<uint16_t> area;
    seen[lockIndex] = 1;
    open.push({0, lockIndex});
    while (!open.empty() && static_cast<int>(area.size()) < size) {
        int i = open.top().second;
        open.pop();
        if (i != lockIndex) {
            const Tile& t = w.tiles[i];
            if (!(ignoreEmpty && t.fg == 0 && t.bg == 0)) area.push_back(static_cast<uint16_t>(i));
        }
        const int x = i % w.width, y = i / w.width;
        const int next[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (auto& n : next) {
            if (n[0] < 0 || n[1] < 0 || n[0] >= w.width || n[1] >= w.height) continue;
            int j = n[1] * w.width + n[0];
            if (seen[j]) continue;
            seen[j] = 1;
            const Tile& t = w.tiles[j];
            if (t.lockParent && t.lockParent != lockIndex) continue;  // inside another lock's area
            const ItemDef* def = GetItem(t.fg);
            if (def && IsLock(*def)) continue;
            open.push({dist(j), j});
        }
    }
    return area;
}

void FindWorldLock(World& w) {
    w.worldLock = -1;
    for (size_t i = 0; i < w.tiles.size(); i++) {
        const ItemDef* def = GetItem(w.tiles[i].fg);
        if (def && IsLock(*def) && def->lockSize == 0) {
            w.worldLock = static_cast<int>(i);
            return;
        }
    }
}

// Own file format, unrelated to the wire format: "BWLD", u32 version, u16 width,
// u16 height, s32 + str (the world owner in version 1, unused since),
// u32 lastObjectID, tiles (u16 fg, u16 bg, s32 owner, s64 plantedAt, u8 fruit,
// str label; version 2 adds str ownerName, u16 lockParent, u8 ignoreEmpty,
// u8 access count and that many s32 userID + str name), u32 drop count and
// the drops.
namespace {
constexpr uint32_t kVersion = 2;

std::string PathFor(const std::string& dir, const std::string& name) { return dir + "/" + name + ".bwld"; }
}  // namespace

bool SaveWorld(const std::string& dir, const World& w) {
    Writer out;
    out.raw("BWLD", 4);
    out.u32(kVersion);
    out.u16(static_cast<uint16_t>(w.width));
    out.u16(static_cast<uint16_t>(w.height));
    out.i32(0);
    out.str16("");
    out.u32(w.lastObjectID);
    for (const Tile& t : w.tiles) {
        out.u16(t.fg);
        out.u16(t.bg);
        out.i32(t.owner);
        out.raw(&t.plantedAt, 8);
        out.u8(t.fruit);
        out.str16(t.label);
        out.str16(t.ownerName);
        out.u16(t.lockParent);
        out.u8(t.ignoreEmpty ? 1 : 0);
        const size_t n = std::min<size_t>(t.access.size(), 255);
        out.u8(static_cast<uint8_t>(n));
        for (size_t i = 0; i < n; i++) {
            out.i32(t.access[i].first);
            out.str16(t.access[i].second);
        }
    }
    out.u32(static_cast<uint32_t>(w.drops.size()));
    for (const DroppedItem& d : w.drops) {
        out.u16(d.item);
        out.f32(d.x);
        out.f32(d.y);
        out.u8(d.count);
        out.u8(d.flags);
        out.u32(d.id);
    }

    std::string tmp = PathFor(dir, w.name) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(out.buf.data(), 1, out.size(), f) == out.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) return false;
    std::string final = PathFor(dir, w.name);
    remove(final.c_str());
    return rename(tmp.c_str(), final.c_str()) == 0;
}

bool LoadWorld(const std::string& dir, World& w) {
    FILE* f = fopen(PathFor(dir, w.name).c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> data;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) data.insert(data.end(), chunk, chunk + n);
    fclose(f);

    Reader in(data.data(), data.size());
    if (data.size() < 8 || memcmp(data.data(), "BWLD", 4) != 0) return false;
    in.get<uint32_t>();
    const uint32_t version = in.get<uint32_t>();
    if (version < 1 || version > kVersion) return false;
    World loaded;
    loaded.name = w.name;
    loaded.width = in.get<uint16_t>();
    loaded.height = in.get<uint16_t>();
    const int32_t v1Owner = in.get<int32_t>();
    const std::string v1OwnerName = in.str16();
    loaded.lastObjectID = in.get<uint32_t>();
    if (!in.ok() || loaded.width <= 0 || loaded.height <= 0 || loaded.width > 255 || loaded.height > 255) return false;
    loaded.tiles.resize(static_cast<size_t>(loaded.width) * loaded.height);
    for (Tile& t : loaded.tiles) {
        t.fg = in.get<uint16_t>();
        t.bg = in.get<uint16_t>();
        t.owner = in.get<int32_t>();
        t.plantedAt = in.get<int64_t>();
        t.fruit = in.get<uint8_t>();
        t.label = in.str16();
        if (version >= 2) {
            t.ownerName = in.str16();
            t.lockParent = in.get<uint16_t>();
            t.ignoreEmpty = in.get<uint8_t>() != 0;
            const uint8_t n = in.get<uint8_t>();
            for (uint8_t i = 0; i < n && in.ok(); i++) {
                const int32_t id = in.get<int32_t>();
                t.access.push_back({id, in.str16()});
            }
        } else if (t.owner && t.owner == v1Owner) {
            t.ownerName = v1OwnerName;
        }
    }
    uint32_t drops = in.get<uint32_t>();
    for (uint32_t i = 0; i < drops && in.ok(); i++) {
        DroppedItem d;
        d.item = in.get<uint16_t>();
        d.x = in.get<float>();
        d.y = in.get<float>();
        d.count = in.get<uint8_t>();
        d.flags = in.get<uint8_t>();
        d.id = in.get<uint32_t>();
        loaded.drops.push_back(d);
    }
    if (!in.ok()) return false;
    FindWorldLock(loaded);
    w = std::move(loaded);
    return true;
}

void SerializeTile(Writer& out, const Tile& t) {
    const ItemDef* fg = GetItem(t.fg);
    uint8_t extra = fg ? ExtraTypeFor(fg->material) : 0;
    const uint16_t flags = (extra ? 1 : 0) | (t.lockParent ? 2 : 0);
    out.u16(t.fg);
    out.u16(t.bg);
    out.u16(t.lockParent);
    out.u16(flags);
    if (t.lockParent) out.u16(t.lockParent);
    if (!extra) return;
    out.u8(extra);
    switch (extra) {
    case EXTRA_DOOR:
        out.str16(t.label);
        out.u8(0);
        break;
    case EXTRA_SIGN:
        out.str16(t.label);
        out.u32(0);
        break;
    case EXTRA_LOCK:
        out.u8(0);
        out.u32(static_cast<uint32_t>(t.owner));
        out.u32(static_cast<uint32_t>(t.access.size()));
        for (auto& a : t.access) out.u32(static_cast<uint32_t>(a.first));
        break;
    case EXTRA_TREE: {
        int64_t grown = t.plantedAt ? static_cast<int64_t>(time(nullptr)) - t.plantedAt : 0;
        out.u32(static_cast<uint32_t>(grown < 0 ? 0 : grown));
        out.u8(t.fruit);
        break;
    }
    }
}

std::vector<uint8_t> SerializeWorld(const World& w) {
    Writer out;
    out.u16(0x0f);  // version, never checked by the client
    out.u32(0);     // flags
    out.str16(w.name);
    out.u32(static_cast<uint32_t>(w.width));
    out.u32(static_cast<uint32_t>(w.height));
    out.u32(static_cast<uint32_t>(w.tiles.size()));
    for (const Tile& t : w.tiles) SerializeTile(out, t);
    out.u32(static_cast<uint32_t>(w.drops.size()));
    out.u32(w.lastObjectID);
    for (const DroppedItem& d : w.drops) {
        out.u16(d.item);
        out.f32(d.x);
        out.f32(d.y);
        out.u8(d.count);
        out.u8(d.flags);
        out.u32(d.id);
    }
    return out.buf;
}
