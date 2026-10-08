// Game logic for the Buildo client. Packet layouts and the order the client
// expects things in are written up in docs/BUILDO_V001_SERVER.md.
#include "game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <random>
#include <sstream>

#include "accounts.h"
#include "items.h"
#include "log.h"
#include "server.h"

Options g_opt;

std::map<ENetPeer*, std::unique_ptr<Player>> g_players;
std::map<std::string, std::unique_ptr<World>> g_worlds;
int g_nextNetID = 1;
// The middle swatch of the client's skin picker (0x428af5).
constexpr uint32_t kDefaultSkin = 0x8295C3FF;
std::mt19937 g_rng{std::random_device{}()};

uint32_t NowMs() { return enet_time_get(); }
int Rand(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g_rng); }

std::string WorldDir() { return g_opt.dataDir + "/worlds"; }

// ---------------------------------------------------------------- sending

void Send(ENetPeer* peer, uint32_t msgType, const void* data, size_t len) {
    // One extra byte on the end: the client zeroes the last byte of every
    // packet before reading it as text (0x43c620).
    ENetPacket* p = enet_packet_create(nullptr, 4 + len + 1, ENET_PACKET_FLAG_RELIABLE);
    memcpy(p->data, &msgType, 4);
    if (len) memcpy(p->data + 4, data, len);
    p->data[4 + len] = 0;
    enet_peer_send(peer, 0, p);
}

void SendTank(ENetPeer* peer, TankPacket t, const std::vector<uint8_t>& ext) {
    std::vector<uint8_t> buf(sizeof(TankPacket) + ext.size());
    if (!ext.empty()) {
        t.flags |= TANK_FLAG_EXTENDED;
        t.extSize = static_cast<uint32_t>(ext.size());
        memcpy(buf.data() + sizeof(TankPacket), ext.data(), ext.size());
    }
    memcpy(buf.data(), &t, sizeof(TankPacket));
    Send(peer, MSG_GAME_PACKET, buf.data(), buf.size());
}

// Message type 3 is the only text the client accepts from a server (0x4324c0).
void SendAction(ENetPeer* peer, const std::string& text) { Send(peer, MSG_GAME_MESSAGE, text.data(), text.size()); }

void Call(ENetPeer* peer, const VariantList& v, int netID, int delayMs) {
    TankPacket t;
    t.type = TANK_CALL_FUNCTION;
    t.netID = netID;
    // Global calls run now with delay -1; avatar calls always go through the
    // client's timer queue, where -1 is not special (0x45cef0).
    t.intData = netID == -1 ? delayMs : std::max(delayMs, 0);
    SendTank(peer, t, v.bytes());
}

void Console(Player& p, const std::string& msg) { Call(p.peer, VariantList("OnConsoleMessage").str(msg)); }

void Dialog(Player& p, const std::string& text) { Call(p.peer, VariantList("OnDialogRequest").str(text)); }

void SendGems(Player& p) { Call(p.peer, VariantList("OnSetBux").i(p.gems)); }

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}

Player* OnlineByName(const std::string& name) {
    std::string want = Lower(name);
    for (auto& [peer, o] : g_players)
        if (o->loggedIn && Lower(o->name) == want) return o.get();
    return nullptr;
}

void SendWorldTank(World* w, const TankPacket& t, const std::vector<uint8_t>& ext) {
    ForWorld(w, [&](Player& p) { SendTank(p.peer, t, ext); });
}

// ---------------------------------------------------------------- player state

void SendInventory(Player& p) {
    Writer w;
    w.u8(1);
    w.u8(static_cast<uint8_t>(std::min<size_t>(p.inventory.size(), 255)));
    for (const InvItem& it : p.inventory) {
        w.u16(it.id);
        w.u8(it.count);
        w.u8(p.Wearing(it.id) ? 1 : 0);
    }
    TankPacket t;
    t.type = TANK_INVENTORY;
    SendTank(p.peer, t, w.buf);
}

VariantList ClothingCall(const Player& p, bool sound) {
    VariantList v("OnSetClothing");
    v.vec3(p.clothes[0], p.clothes[1], p.clothes[2]);
    v.vec3(p.clothes[3], p.clothes[4], p.clothes[5]);
    v.u(p.skin);
    v.u(sound ? 1 : 0);
    return v;
}

std::string SpawnText(const Player& p, bool local) {
    std::ostringstream s;
    s << "spawn|avatar\nnetID|" << p.netID << "\nuserID|" << p.userID << "\ncolrect|0|0|20|30\nposXY|"
      << static_cast<int>(p.x) << "|" << static_cast<int>(p.y) << "\nname|" << p.name << "\ncountry|" << p.country
      << "\n";
    if (local) s << "type|local\n";
    return s.str();
}

void GiveStarterKit(Player& p) {
    p.inventory = {{ITEM_FIST, 1}, {ITEM_WRENCH, 1}, {ITEM_DIRT, 50}, {ITEM_DIRT + 1, 10}, {ITEM_ROCK, 20},
                   {ITEM_CAVE_WALL, 50}};
    for (uint16_t id : {52, 54, 20, 12, 60, 202, 62, 100}) {
        if (GetItem(id)) p.inventory.push_back({id, static_cast<uint8_t>(GetItem(id)->material == 3 ? 1 : 20)});
    }
    for (uint16_t id : {36, 48, 40, 68}) {
        if (GetItem(id)) {
            p.inventory.push_back({id, 1});
            p.clothes[GetItem(id)->bodyPart] = id;
        }
    }
}

// ---------------------------------------------------------------- worlds

World* GetWorld(const std::string& name) {
    auto it = g_worlds.find(name);
    if (it != g_worlds.end()) return it->second.get();
    auto w = std::make_unique<World>();
    w->name = name;
    if (!LoadWorld(WorldDir(), *w)) {
        GenerateWorld(*w);
        Log("generated world %s", name.c_str());
    }
    // Area locks saved before they had an area (or before their size changed
    // in extra_items.txt) claim one now; the client gets it with the map.
    for (size_t i = 0; i < w->tiles.size(); i++) {
        const ItemDef* def = GetItem(w->tiles[i].fg);
        if (!def || !IsLock(*def) || def->lockSize <= 0) continue;
        int lockIndex = static_cast<int>(i);
        if (std::any_of(w->tiles.begin(), w->tiles.end(), [&](const Tile& t) { return t.lockParent == lockIndex; }))
            continue;
        for (uint16_t a : ComputeLockArea(*w, lockIndex, def->lockSize, w->tiles[i].ignoreEmpty))
            w->tiles[a].lockParent = static_cast<uint16_t>(lockIndex);
        w->dirty = true;
    }
    World* raw = w.get();
    g_worlds[name] = std::move(w);
    return raw;
}

void SaveWorldIfDirty(World& w) {
    if (!w.dirty) return;
    if (SaveWorld(WorldDir(), w)) w.dirty = false;
    else LogError("could not save world %s", w.name.c_str());
}

int PlayersIn(World* w) {
    int n = 0;
    ForWorld(w, [&](Player&) { n++; });
    return n;
}

// ---------------------------------------------------------------- locks

// The lock that decides who may change a tile: the tile itself when it is a
// lock, else the area lock covering it, else the world lock.
const Tile* GoverningLock(const World& w, int x, int y) {
    const Tile& t = w.tiles[w.Index(x, y)];
    const ItemDef* def = GetItem(t.fg);
    if (def && IsLock(*def)) return &t;
    if (t.lockParent) return &w.tiles[t.lockParent];
    if (w.worldLock >= 0) return &w.tiles[w.worldLock];
    return nullptr;
}

bool HasAccess(const Player& p, const Tile& lock) {
    if (lock.owner == p.userID) return true;
    for (auto& a : lock.access)
        if (a.first == p.userID) return true;
    return false;
}

bool CanEdit(const Player& p, const World& w, int x, int y) {
    const Tile* lock = GoverningLock(w, x, y);
    return !lock || HasAccess(p, *lock);
}

bool IsWorldLockItem(const ItemDef& def) { return IsLock(def) && def.lockSize <= 0; }

void LockedOut(Player& p, const World& w, int x, int y) {
    SendAction(p.peer, "action|play_sfx\nfile|audio/punch_locked.wav\ndelayMS|0\n");
    uint32_t now = NowMs();
    if (now - p.lastLockMsgMs < 3000) return;
    p.lastLockMsgMs = now;
    const Tile* lock = GoverningLock(w, x, y);
    std::string owner = lock && !lock->ownerName.empty() ? lock->ownerName : "someone else";
    Console(p, "`4That's locked by `w" + owner + "``.``");
}

std::vector<uint16_t> AreaOf(const World& w, int lockIndex) {
    std::vector<uint16_t> area;
    for (size_t i = 0; i < w.tiles.size(); i++)
        if (w.tiles[i].lockParent == lockIndex) area.push_back(static_cast<uint16_t>(i));
    return area;
}

// Tank 15 (0x441070): sets the lock tile and its owner, unlocks the tiles it
// held before (0x440cb0), then marks the listed tile indices as locked by it.
void SendLock(World& w, int lockIndex) {
    const Tile& lock = w.tiles[lockIndex];
    std::vector<uint16_t> area = AreaOf(w, lockIndex);
    Writer out;
    for (uint16_t i : area) out.u16(i);
    TankPacket t;
    t.type = TANK_LOCK;
    t.netID = lock.owner;
    t.item = static_cast<int32_t>(area.size());
    t.intData = lock.fg;
    t.tileX = lockIndex % w.width;
    t.tileY = lockIndex / w.width;
    SendWorldTank(&w, t, out.buf);
}

int ApplyAreaLock(World& w, int lockIndex) {
    const ItemDef* def = GetItem(w.tiles[lockIndex].fg);
    for (Tile& t : w.tiles)
        if (t.lockParent == lockIndex) t.lockParent = 0;
    auto area = ComputeLockArea(w, lockIndex, def ? def->lockSize : 0, w.tiles[lockIndex].ignoreEmpty);
    for (uint16_t i : area) w.tiles[i].lockParent = static_cast<uint16_t>(lockIndex);
    w.dirty = true;
    SendLock(w, lockIndex);
    return static_cast<int>(area.size());
}

void ReleaseLock(World& w, int lockIndex) {
    for (Tile& t : w.tiles)
        if (t.lockParent == lockIndex) t.lockParent = 0;
    if (w.worldLock == lockIndex) w.worldLock = -1;
    Tile& lock = w.tiles[lockIndex];
    lock.owner = 0;
    lock.ownerName.clear();
    lock.access.clear();
    lock.ignoreEmpty = false;
}

// Looks a player up by name, online first, then saved accounts.
bool FindPlayer(const std::string& rawName, int& userID, std::string& name) {
    std::string want;
    for (char c : rawName)
        if (isalnum(static_cast<unsigned char>(c)) || c == '_') want += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (want.empty()) return false;
    for (auto& [peer, o] : g_players) {
        std::string lower = o->name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (o->loggedIn && lower == want) {
            userID = o->userID;
            name = o->name;
            return true;
        }
    }
    Account acc;
    if (!LoadAccount(g_opt.dataDir, want, acc)) return false;
    userID = acc.userID;
    name = acc.name.empty() ? want : acc.name;
    return true;
}

Player* OnlineByUserID(int userID) {
    for (auto& [peer, o] : g_players)
        if (o->loggedIn && o->userID == userID) return o.get();
    return nullptr;
}

void SpawnPoint(World& w, float& x, float& y) {
    int dx = w.width / 2, dy = 0;
    w.FindDoor(dx, dy);
    x = static_cast<float>(dx * 32);
    y = static_cast<float>(dy * 32);
}

void LeaveWorld(Player& p, bool toMenu) {
    World* w = p.world;
    if (!w) return;
    CancelTrade(p, "left the world");
    p.world = nullptr;
    std::string remove = "netID|" + std::to_string(p.netID) + "\n";
    ForWorld(w, [&](Player& o) {
        Call(o.peer, VariantList("OnRemove").str(remove));
        Console(o, "`5<`w" + p.name + "`` left, `w" + std::to_string(PlayersIn(w)) + "`` others here>``");
    });
    SaveWorldIfDirty(*w);
    if (toMenu) {
        Call(p.peer, VariantList("OnRequestWorldSelectMenu").str(""));
        ShowWorldList(p);
    }
}

void JoinWorld(Player& p, const std::string& rawName) {
    std::string name = NormalizeWorldName(rawName);
    if (name.empty()) {
        Console(p, "That world name won't work. Use letters and numbers only, up to 24.");
        Call(p.peer, VariantList("OnFailedToEnterWorld"));
        return;
    }
    World* w = GetWorld(name);
    if (IsWorldBanned(p, *w)) {
        Console(p, "`4You're banned from `w" + name + "``.``");
        Call(p.peer, VariantList("OnFailedToEnterWorld"));
        return;
    }
    if (p.world) LeaveWorld(p, false);
    RememberWorld(p, name);
    TankPacket map;
    map.type = TANK_MAP_DATA;
    SendTank(p.peer, map, SerializeWorld(*w));

    SpawnPoint(*w, p.x, p.y);
    p.world = w;
    Call(p.peer, VariantList("OnSpawn").str(SpawnText(p, true)));
    Call(p.peer, ClothingCall(p, false), p.netID);

    int others = 0;
    ForWorld(w, [&](Player& o) {
        if (&o == &p) return;
        others++;
        Call(p.peer, VariantList("OnSpawn").str(SpawnText(o, false)));
        Call(p.peer, ClothingCall(o, false), o.netID);
        Call(o.peer, VariantList("OnSpawn").str(SpawnText(p, false)));
        Call(o.peer, ClothingCall(p, false), p.netID);
        Console(o, "`5<`w" + p.name + "`` entered, `w" + std::to_string(PlayersIn(w) - 1) + "`` others here>``");
    });
    std::string owner = w->worldLock < 0 ? ""
                        : " `0" + w->name + "`` is locked by `w" + w->tiles[w->worldLock].ownerName + "``.";
    Console(p, "World `w" + w->name + "`` entered. There " + (others == 1 ? "is `w1`` other" : "are `w" +
               std::to_string(others) + "`` others") + " here." + owner);
    Log("%s joined %s", p.name.c_str(), w->name.c_str());
}

// ---------------------------------------------------------------- dropped items

void DropItem(World& w, uint16_t item, int count, float x, float y) {
    DroppedItem d;
    d.item = item;
    d.count = static_cast<uint8_t>(count);
    d.x = x;
    d.y = y;
    d.id = ++w.lastObjectID;  // every client in the world bumps its own counter the same way
    w.drops.push_back(d);
    w.dirty = true;

    TankPacket t;
    t.type = TANK_ITEM_OBJECT;
    t.netID = -1;
    t.intData = item;
    t.posX = x;
    t.posY = y;
    t.floatVar = static_cast<float>(count);
    SendWorldTank(&w, t);
}

void PickUp(Player& p, uint32_t objectID) {
    World* w = p.world;
    if (!w) return;
    auto it = std::find_if(w->drops.begin(), w->drops.end(), [&](const DroppedItem& d) { return d.id == objectID; });
    if (it == w->drops.end()) return;
    float dx = it->x - p.x, dy = it->y - p.y;
    if (dx * dx + dy * dy > 96.f * 96.f) return;

    bool gems = it->item == ITEM_GEMS;
    if (gems) {
        p.gems += it->count;
    } else if (!p.Add(it->item, it->count)) {
        Console(p, "You can't carry any more of that.");
        return;
    }
    TankPacket t;
    t.type = TANK_ITEM_OBJECT;
    t.netID = p.netID;
    t.intData = static_cast<int32_t>(it->id);
    w->drops.erase(it);
    w->dirty = true;
    SendWorldTank(w, t);
    if (gems) Call(p.peer, VariantList("OnSetBux").i(p.gems));
}

// ---------------------------------------------------------------- tiles

void SendTileUpdate(World& w, int x, int y) {
    Writer out;
    SerializeTile(out, *w.At(x, y));
    TankPacket t;
    t.type = TANK_TILE_UPDATE;
    t.tileX = x;
    t.tileY = y;
    SendWorldTank(&w, t, out.buf);
}

void BreakDrops(World& w, uint16_t broken, int x, int y) {
    float cx = x * 32.f + 8.f, cy = y * 32.f + 8.f;
    if (broken == ITEM_TREASURE_CHEST) {
        for (int i = 0; i < 4; i++) DropItem(w, ITEM_GEMS, Rand(5, 30), cx + Rand(-8, 8), cy + Rand(-8, 8));
        return;
    }
    if (GetItem(ITEM_TREASURE_CHEST) && Rand(1, 250) == 1) {
        DropItem(w, ITEM_TREASURE_CHEST, 1, cx, cy);
        return;
    }
    const ItemDef* seed = GetItem(broken + 1u);
    if (seed && IsSeedItem(*seed) && Rand(1, 100) <= 30)
        DropItem(w, broken + 1, 1, cx + Rand(-6, 6), cy + Rand(-6, 6));
    if (Rand(1, 100) <= 15) DropItem(w, broken, 1, cx + Rand(-6, 6), cy + Rand(-6, 6));
    if (Rand(1, 100) <= 40) DropItem(w, ITEM_GEMS, Rand(1, 3), cx + Rand(-6, 6), cy + Rand(-6, 6));
}

void HarvestTree(Player& p, World& w, Tile& t, int x, int y) {
    const ItemDef* seed = GetItem(t.fg);
    uint16_t fruit = static_cast<uint16_t>(t.fg - 1);
    int count = std::max<int>(1, t.fruit);
    TankPacket h;
    h.type = TANK_TREE_STATE;
    h.netID = p.netID;
    h.item = -1;  // +0x08 = -1: harvested, clear the tile
    h.tileX = x;
    h.tileY = y;
    SendWorldTank(&w, h);
    t.fg = 0;
    t.plantedAt = 0;
    t.fruit = 0;
    w.dirty = true;
    float cx = x * 32.f + 8.f, cy = y * 32.f + 8.f;
    DropItem(w, fruit, count, cx, cy);
    if (seed && Rand(1, 100) <= 50) DropItem(w, seed->id, 1, cx + 6, cy);
}

bool TreeGrown(const Tile& t) {
    const ItemDef* seed = GetItem(t.fg);
    if (!seed || !t.plantedAt) return false;
    return static_cast<int64_t>(time(nullptr)) - t.plantedAt >= static_cast<int64_t>(seed->growSeconds);
}

void Punch(Player& p, World& w, int x, int y) {
    Tile& t = *w.At(x, y);
    uint16_t target = t.fg ? t.fg : t.bg;
    if (!target) return;
    const ItemDef* def = GetItem(target);
    if (!def || IsUnbreakable(*def)) return;
    // Access lets you build in a lock's area, but only the owner breaks the lock.
    if (!CanEdit(p, w, x, y) || (t.fg && IsLock(*def) && t.owner != p.userID)) {
        LockedOut(p, w, x, y);
        return;
    }
    if (t.fg && IsSeedItem(*def) && TreeGrown(t)) {
        HarvestTree(p, w, t, x, y);
        return;
    }

    uint32_t now = NowMs();
    if (now - t.lastHitMs > def->healSeconds * 1000u) t.damage = 0;
    t.lastHitMs = now;
    t.damage++;
    if (t.damage < std::max<uint8_t>(def->hp, 1)) {
        TankPacket d;
        d.type = TANK_TILE_DAMAGE;
        d.netID = p.netID;
        d.intData = 1;
        d.tileX = x;
        d.tileY = y;
        SendWorldTank(&w, d);
        return;
    }

    // Broken: an echoed fist tile change clears the top layer on every client.
    TankPacket b;
    b.type = TANK_TILE_CHANGE;
    b.netID = p.netID;
    b.intData = ITEM_FIST;
    b.tileX = x;
    b.tileY = y;
    SendWorldTank(&w, b);
    t.damage = 0;
    if (t.fg) {
        // The client unlocks the area itself when the lock tile is replaced (0x43e770).
        if (IsLock(*def)) ReleaseLock(w, w.Index(x, y));
        t.fg = 0;
        t.label.clear();
        t.plantedAt = 0;
        t.fruit = 0;
    } else {
        t.bg = 0;
    }
    w.dirty = true;
    if (IsLock(*def)) DropItem(w, target, 1, x * 32.f + 8.f, y * 32.f + 8.f);  // locks always come back
    else if (!IsSeedItem(*def)) BreakDrops(w, target, x, y);
}

// A seed planted on a tree that hasn't grown yet splices the two (seed1 and
// seed2 of the result seed in items.dat).
void Splice(Player& p, World& w, int x, int y, uint16_t seed) {
    Tile& t = *w.At(x, y);
    const ItemDef* a = GetItem(t.fg);
    const ItemDef* b = GetItem(seed);
    uint16_t result = SpliceResult(t.fg, seed);
    if (!result) {
        Console(p, "`4Hmm, `w" + a->name + "`` and `w" + b->name + "`` won't splice.``");
        return;
    }
    const ItemDef* r = GetItem(result);
    t.fg = result;
    t.plantedAt = static_cast<int64_t>(time(nullptr));
    t.fruit = static_cast<uint8_t>(Rand(1, std::max<int>(r->maxFruit, 1)));
    w.dirty = true;
    p.Remove(seed, 1);
    TankPacket rm;
    rm.type = TANK_REMOVE_ITEM;
    rm.pad2 = 1;
    rm.intData = seed;
    SendTank(p.peer, rm);
    SendTileUpdate(w, x, y);
    ForWorld(&w, [&](Player& o) { SendAction(o.peer, "action|play_sfx\nfile|audio/tree_plant.wav\ndelayMS|0\n"); });
    Console(p, "`5You spliced `w" + a->name + "`` with `w" + b->name + "`` into a `w" + r->name + "``!``");
}

void Place(Player& p, World& w, int x, int y, uint16_t item) {
    const ItemDef* def = GetItem(item);
    if (!def || IsClothes(*def) || item == ITEM_GEMS || p.Count(item) <= 0) return;
    Tile& t = *w.At(x, y);
    const int index = w.Index(x, y);
    if (!CanEdit(p, w, x, y)) {
        LockedOut(p, w, x, y);
        return;
    }
    if (IsBackground(*def)) {
        if (t.bg) return;
        t.bg = item;
    } else {
        if (t.fg) {
            const ItemDef* cur = GetItem(t.fg);
            if (IsSeedItem(*def) && cur && IsSeedItem(*cur) && !TreeGrown(t)) Splice(p, w, x, y, item);
            return;
        }
        if (IsLock(*def)) {
            if (index == 0) return;  // index 0 means "no lock" in a tile's parent field
            if (t.lockParent) {
                Console(p, "That spot is already inside a lock's area.");
                return;
            }
            if (IsWorldLockItem(*def)) {
                if (w.worldLock >= 0) {
                    Console(p, "This world is already locked.");
                    return;
                }
                for (const Tile& o : w.tiles) {
                    const ItemDef* od = GetItem(o.fg);
                    if (od && IsLock(*od) && o.owner != p.userID) {
                        Console(p, "Someone else has a lock in this world, so you can't lock all of it.");
                        return;
                    }
                }
            }
            t.owner = p.userID;
            t.ownerName = p.name;
            t.access.clear();
            t.ignoreEmpty = false;
        }
        t.fg = item;
        t.label.clear();
        if (IsSeedItem(*def)) {
            t.plantedAt = static_cast<int64_t>(time(nullptr));
            t.fruit = static_cast<uint8_t>(Rand(1, std::max<int>(def->maxFruit, 1)));
        }
    }
    w.dirty = true;

    // The placer's client takes the item out of its own inventory when this
    // comes back with its netID (0x433c95), so no type 13 here.
    TankPacket c;
    c.type = TANK_TILE_CHANGE;
    c.netID = p.netID;
    c.intData = item;
    c.tileX = x;
    c.tileY = y;
    if (IsSeedItem(*def)) c.pad3 = t.fruit;  // the client's fruit count for the new tree (0x43f30b)
    SendWorldTank(&w, c);
    if (def->maxCanHold != 0) p.Remove(item, 1);

    if (IsLock(*def)) {
        if (IsWorldLockItem(*def)) {
            w.worldLock = index;
            SendLock(w, index);
            ForWorld(&w, [&](Player& o) { Console(o, "`5[`w" + p.name + "`` has locked `w" + w.name + "``]``"); });
        } else {
            int n = ApplyAreaLock(w, index);
            Console(p, "`5Area locked: `w" + std::to_string(n) + "`` tiles. Wrench the lock to share it.``");
        }
    }
}

void LockDialog(Player& p, World& w, int x, int y) {
    const Tile& t = *w.At(x, y);
    const ItemDef* def = GetItem(t.fg);
    std::string scope = IsWorldLockItem(*def) ? "Locks the whole world."
                                              : "Locks `w" + std::to_string(AreaOf(w, w.Index(x, y)).size()) +
                                                    "`` tiles around it (up to " + std::to_string(def->lockSize) + ").";
    std::string names;
    for (auto& a : t.access) names += (names.empty() ? "" : ", ") + a.second;
    std::string head = "set_default_color|`o\nadd_label_with_icon|big|`w" + def->name + "``|left|" +
                       std::to_string(t.fg) + "|\nadd_textbox|Owned by `w" + t.ownerName + "``. " + scope + "|left|\n";
    if (t.owner != p.userID) {
        std::string mine = HasAccess(p, t) ? "You have access, so you can build here." : "You don't have access.";
        Call(p.peer, VariantList("OnDialogRequest").str(head + "add_textbox|" + mine + "|left|\nend_dialog|lock_info|Close||\n"));
        return;
    }
    std::string d = head;
    d += "add_textbox|" + (names.empty() ? std::string("Nobody else can build here.") : "Access: `w" + names + "``") + "|left|\n";
    d += "add_text_input|add_name|Give access to|" + std::string() + "|18|\n";
    if (!t.access.empty()) d += "add_checkbox|clear_access|Take everyone's access away|0|\n";
    if (!IsWorldLockItem(*def)) {
        d += "add_checkbox|ignore_empty|Ignore empty air|" + std::string(t.ignoreEmpty ? "1" : "0") + "|\n";
        d += "add_checkbox|reapply|Re-apply the lock to what's around it now|0|\n";
    }
    d += "embed_data|tilex|" + std::to_string(x) + "\nembed_data|tiley|" + std::to_string(y) + "\n";
    d += "end_dialog|lock_edit|Cancel|OK|\n";
    Call(p.peer, VariantList("OnDialogRequest").str(d));
}

void LockEdit(Player& p, World& w, int x, int y, std::map<std::string, std::string>& kv) {
    Tile& t = *w.At(x, y);
    const ItemDef* def = GetItem(t.fg);
    if (!def || !IsLock(*def) || t.owner != p.userID) return;
    const int index = w.Index(x, y);
    bool changed = false;
    if (kv["clear_access"] == "1" && !t.access.empty()) {
        t.access.clear();
        changed = true;
        Console(p, "Everyone's access to that lock is gone.");
    }
    if (!kv["add_name"].empty()) {
        int id = 0;
        std::string name;
        if (!FindPlayer(kv["add_name"], id, name)) {
            Console(p, "Nobody called `w" + kv["add_name"] + "`` has played here.");
        } else if (id == p.userID) {
            Console(p, "You own it already.");
        } else if (std::none_of(t.access.begin(), t.access.end(), [&](auto& a) { return a.first == id; })) {
            t.access.push_back({id, name});
            changed = true;
            Console(p, "`w" + name + "`` can build here now.");
            if (Player* other = OnlineByUserID(id))
                Console(*other, "`5" + p.name + " gave you access to a " + def->name + " in `w" + w.name + "``.``");
        }
    }
    if (!IsWorldLockItem(*def)) {
        bool ignore = kv["ignore_empty"] == "1";
        if (ignore != t.ignoreEmpty || kv["reapply"] == "1") {
            t.ignoreEmpty = ignore;
            int n = ApplyAreaLock(w, index);
            Console(p, "`5Area locked: `w" + std::to_string(n) + "`` tiles.``");
        }
    }
    if (changed) {
        w.dirty = true;
        // The new access list travels in the lock's tile extra. A tile update
        // re-creates the lock tile, which drops its area on the client, so the
        // area goes out again after it.
        SendTileUpdate(w, x, y);
        SendLock(w, index);
    }
}

void Wrench(Player& p, World& w, int x, int y) {
    Tile& t = *w.At(x, y);
    const ItemDef* def = GetItem(t.fg);
    if (!def) return;
    std::string xy = "embed_data|tilex|" + std::to_string(x) + "\nembed_data|tiley|" + std::to_string(y) + "\n";
    if (IsSign(*def)) {
        if (!CanEdit(p, w, x, y)) return;
        Call(p.peer, VariantList("OnDialogRequest").str(
                         "set_default_color|`o\nadd_label_with_icon|big|`wEdit " + def->name + "``|left|" +
                         std::to_string(t.fg) + "|\nadd_text_input|text|What should it say?|" + t.label +
                         "|100|\n" + xy + "end_dialog|sign_edit|Cancel|OK|\n"));
    } else if (IsDoor(*def) && !IsMainDoor(*def)) {
        if (!CanEdit(p, w, x, y)) return;
        Call(p.peer, VariantList("OnDialogRequest").str(
                         "set_default_color|`o\nadd_label_with_icon|big|`wEdit " + def->name + "``|left|" +
                         std::to_string(t.fg) + "|\nadd_textbox|Type a world name to make this door go there.|left|\n"
                         "add_text_input|dest|Destination|" + t.label + "|24|\n" + xy +
                         "end_dialog|door_edit|Cancel|OK|\n"));
    } else if (IsLock(*def)) {
        LockDialog(p, w, x, y);
    }
}

void OnTileChange(Player& p, const TankPacket& in) {
    World* w = p.world;
    if (!w || !w->At(in.tileX, in.tileY)) return;
    const ItemDef* def = GetItem(in.intData);
    if (!def || p.Count(static_cast<uint16_t>(in.intData)) <= 0) return;
    if (IsFist(*def)) Punch(p, *w, in.tileX, in.tileY);
    else if (IsWrench(*def)) Wrench(p, *w, in.tileX, in.tileY);
    else Place(p, *w, in.tileX, in.tileY, static_cast<uint16_t>(in.intData));
}

void OnTileActivate(Player& p, const TankPacket& in) {
    World* w = p.world;
    if (!w || !w->At(in.tileX, in.tileY)) return;
    Tile& t = *w->At(in.tileX, in.tileY);
    const ItemDef* def = GetItem(t.fg);
    if (def && IsMainDoor(*def)) {
        LeaveWorld(p, true);
        return;
    }
    if (def && IsDoor(*def) && !NormalizeWorldName(t.label).empty() && NormalizeWorldName(t.label) != w->name) {
        JoinWorld(p, t.label);
        return;
    }
    // The client froze itself when it touched the door; let it go again.
    Call(p.peer, VariantList("OnSetFreezeState").u(0), p.netID);
    if (def && IsDoor(*def)) Console(p, "This door doesn't go anywhere yet. Wrench it to set a world.");
}

void OnItemActivate(Player& p, const TankPacket& in) {
    const ItemDef* def = GetItem(in.intData);
    if (!def || !IsClothes(*def) || p.Count(def->id) <= 0) return;
    uint16_t& slot = p.clothes[def->bodyPart];
    slot = slot == def->id ? 0 : def->id;
    if (p.world) ForWorld(p.world, [&](Player& o) { Call(o.peer, ClothingCall(p, &o == &p), p.netID); });
    else SendInventory(p);
}

void OnState(Player& p, const uint8_t* raw) {
    if (!p.world) return;
    TankPacket t;
    memcpy(&t, raw, sizeof t);
    p.x = t.posX;
    p.y = t.posY;
    p.facingLeft = (t.flags & 0x10) != 0;
    t.netID = p.netID;
    t.flags &= ~TANK_FLAG_EXTENDED;
    t.extSize = 0;
    ForWorld(p.world, [&](Player& o) {
        if (&o != &p) SendTank(o.peer, t);
    });
}

// ---------------------------------------------------------------- chat & dialogs

void Respawn(Player& p) {
    if (!p.world) return;
    // OnKilled starts the death animation; the client only ends it when the
    // freeze state goes from 2 back to 0 (0x448a69 -> 0x44a4c0).
    float x, y;
    SpawnPoint(*p.world, x, y);
    ForWorld(p.world, [&](Player& o) {
        Call(o.peer, VariantList("OnKilled"), p.netID);
        Call(o.peer, VariantList("OnSetFreezeState").u(2), p.netID);
        Call(o.peer, VariantList("OnSetPos").vec2(x, y), p.netID, 2000);
        Call(o.peer, VariantList("OnSetFreezeState").u(0), p.netID, 2000);
    });
    p.x = x;
    p.y = y;
}

void Command(Player& p, const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
    if (SocialCommand(p, cmd, in) || ShopCommand(p, cmd, in) || TradeCommand(p, cmd, in)) return;
    if (cmd == "/help" || cmd == "/?") {
        Console(p, "`oCommands: `w/shop``, `w/trade <name>``, `w/worlds``, `w/msg <name> <text>``, `w/r <text>``, "
                   "`w/friends``, `w/addfriend <name>``, `w/unfriend <name>``, `w/who``, `w/wave``, `w/dance``, "
                   "`w/respawn``, `w/gems``, `w/items``, `w/find <name>``, `w/canceltrade``");
        if (p.world && IsWorldOwner(p, *p.world))
            Console(p, "`oWorld owner: `w/kick <name>``, `w/ban <name>``, `w/unban <name>``, `w/bans``");
        if (IsMod(p))
            Console(p, "`oMod: `w/item <id> [count]``, `w/mute <name> <minutes>``, `w/unmute <name>``, `w/sban <name>``");
    } else if (cmd == "/item") {
        if (!IsMod(p)) {
            Console(p, "Only mods can make items. Break blocks for gems and spend them in the /shop.");
            return;
        }
        int id = -1, count = 1;
        in >> id >> count;
        const ItemDef* def = GetItem(id);
        if (!def || id == ITEM_GEMS) {
            Console(p, "No item with that ID. Try /items.");
            return;
        }
        count = std::clamp(count, 1, static_cast<int>(kMaxStack));
        if (!p.Add(static_cast<uint16_t>(id), count)) {
            Console(p, "That won't fit in your backpack.");
            return;
        }
        SendInventory(p);
        Console(p, "Given `w" + std::to_string(count) + " " + def->name + "``.");
    } else if (cmd == "/items" || cmd == "/find") {
        std::string filter;
        std::getline(in, filter);
        filter.erase(0, filter.find_first_not_of(' '));
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
        std::string out;
        for (const ItemDef& it : AllItems()) {
            if (!it.defined || it.name == "Unused" || it.id == ITEM_GEMS) continue;
            std::string lower = it.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
            if (filter.empty() && it.isSeed) continue;
            if (!out.empty()) out += ", ";
            out += std::to_string(it.id) + " " + it.name;
        }
        Console(p, out.empty() ? "Nothing matches." : "`o" + out + "``");
    } else if (cmd == "/who") {
        std::string names;
        ForWorld(p.world, [&](Player& o) { names += (names.empty() ? "" : ", ") + o.name; });
        Console(p, "Here: `w" + names + "``. Online: `w" + std::to_string(g_players.size()) + "``.");
    } else if (cmd == "/wave" || cmd == "/dance") {
        ForWorld(p.world, [&](Player& o) { Call(o.peer, VariantList("OnAction").str(cmd), p.netID); });
    } else if (cmd == "/respawn") {
        Respawn(p);
    } else if (cmd == "/gems") {
        Console(p, "You have `w" + std::to_string(p.gems) + "`` gems.");
    } else {
        Console(p, "Unknown command. Try /help.");
    }
}

void Chat(Player& p, std::string msg) {
    msg.erase(std::remove_if(msg.begin(), msg.end(), [](char c) { return c == '\n' || c == '\r' || c == '`'; }),
              msg.end());
    if (msg.empty() || msg.size() > 120) return;
    if (msg[0] == '/') {
        Command(p, msg);
        return;
    }
    if (!p.world || !ChatAllowed(p, msg)) return;
    ForWorld(p.world, [&](Player& o) {
        Call(o.peer, VariantList("OnTalkBubble").u(static_cast<uint32_t>(p.netID)).str(msg).u(0).u(0));
        Console(o, "`o<`w" + p.name + "``> " + msg + "``");
    });
}

void DialogReturn(Player& p, std::map<std::string, std::string>& kv) {
    if (SocialDialog(p, kv) || ShopDialog(p, kv) || TradeDialog(p, kv)) return;
    const std::string& name = kv["dialog_name"];
    World* w = p.world;
    if (name == "drop_item") {
        int id = atoi(kv["itemID"].c_str());
        int count = atoi(kv["count"].c_str());
        const ItemDef* def = GetItem(id);
        if (!def || !w || count <= 0 || count > p.Count(static_cast<uint16_t>(id))) return;
        float x = p.x + (p.facingLeft ? -28.f : 28.f), y = p.y + 4.f;
        p.Remove(static_cast<uint16_t>(id), count);
        TankPacket r;
        r.type = TANK_REMOVE_ITEM;
        r.pad2 = static_cast<uint8_t>(count);
        r.intData = id;
        SendTank(p.peer, r);
        if (def->material == 14 && !p.Wearing(static_cast<uint16_t>(id)))
            ForWorld(w, [&](Player& o) { Call(o.peer, ClothingCall(p, false), p.netID); });
        DropItem(*w, static_cast<uint16_t>(id), count, x, y);
        return;
    }
    if (!w) return;
    int x = atoi(kv["tilex"].c_str()), y = atoi(kv["tiley"].c_str());
    Tile* t = w->At(x, y);
    if (!t) return;
    if (name == "lock_edit") {
        LockEdit(p, *w, x, y, kv);
        return;
    }
    if (!CanEdit(p, *w, x, y)) return;
    const ItemDef* def = GetItem(t->fg);
    if (!def) return;
    if (name == "sign_edit" && IsSign(*def)) {
        std::string text = kv["text"];
        text.erase(std::remove(text.begin(), text.end(), '`'), text.end());
        t->label = text.substr(0, 100);
    } else if (name == "door_edit" && IsDoor(*def) && !IsMainDoor(*def)) {
        t->label = NormalizeWorldName(kv["dest"]);
    } else {
        return;
    }
    w->dirty = true;
    SendTileUpdate(*w, x, y);
}

std::string SpliceLine(const ItemDef& seed) {
    const ItemDef* a = GetItem(seed.seed1);
    const ItemDef* b = GetItem(seed.seed2);
    if (!a || !b) return "";
    return "Splice `w" + a->name + "`` with `w" + b->name + "`` to get it.";
}

void ItemInfo(Player& p, int id) {
    const ItemDef* def = GetItem(id);
    if (!def) return;
    std::vector<std::string> lines;
    if (IsClothes(*def)) {
        lines.push_back("Something to wear. Double tap it in your inventory to put it on.");
    } else if (IsSeedItem(*def)) {
        lines.push_back("Plant it, wait " + std::to_string(def->growSeconds) + " seconds, then punch the tree.");
        std::string made = SpliceLine(*def);
        lines.push_back(made.empty() ? std::string("You get it from breaking blocks, or from seed packs in the /shop.")
                                     : made);
        std::vector<std::string> with;
        for (const ItemDef& o : AllItems()) {
            if (!o.isSeed || (o.seed1 != id && o.seed2 != id)) continue;
            const ItemDef* other = GetItem(o.seed1 == id ? o.seed2 : o.seed1);
            if (other) with.push_back("+ `w" + other->name + "`` = `w" + o.name + "``");
        }
        if (!with.empty()) {
            lines.push_back("Plant another seed on its sapling to splice:");
            lines.insert(lines.end(), with.begin(), with.end());
        }
    } else {
        if (IsLock(*def))
            lines.push_back(def->lockSize > 0 ? "Locks " + std::to_string(def->lockSize) + " tiles around it."
                                              : "Locks the whole world.");
        else if (IsBackground(*def))
            lines.push_back("A background block.");
        else if (def->hp)
            lines.push_back("Takes " + std::to_string(def->hp) + " hits to break.");
        const ItemDef* seed = GetItem(id + 1u);
        if (seed && IsSeedItem(*seed)) {
            std::string made = SpliceLine(*seed);
            lines.push_back("Grows on the `w" + seed->name + "`` tree." + (made.empty() ? "" : " " + made));
        }
    }
    std::string d = "set_default_color|`o\nadd_label_with_icon|big|`w" + def->name + "``|left|" + std::to_string(id) + "|\n";
    for (auto& l : lines) d += "add_textbox|" + l + "|left|\n";
    d += "end_dialog|info|Close||\n";
    Dialog(p, d);
}

void DropDialog(Player& p, int id) {
    const ItemDef* def = GetItem(id);
    int have = p.Count(static_cast<uint16_t>(id));
    if (!def || have <= 0 || def->maxCanHold == 0 || !p.world) return;
    Call(p.peer, VariantList("OnDialogRequest").str(
                     "set_default_color|`o\nadd_label_with_icon|big|`wDrop " + def->name + "``|left|" +
                     std::to_string(id) + "|\nadd_text_input|count|How many?|" + std::to_string(have) + "|3|\n"
                     "embed_data|itemID|" + std::to_string(id) + "\nend_dialog|drop_item|Cancel|OK|\n"));
}

// ---------------------------------------------------------------- login

void Login(Player& p, std::map<std::string, std::string>& kv) {
    std::string growID = kv["tankIDName"];
    std::string requested = growID.empty() ? kv["requestedName"] : growID;
    std::string name;
    for (char c : requested)
        if (isalnum(static_cast<unsigned char>(c)) && name.size() < 18) name += c;
    if (name.size() < 3) name = "Guest";
    p.country = kv["country"].empty() ? "us" : kv["country"].substr(0, 2);

    Account acc;
    bool exists = LoadAccount(g_opt.dataDir, name, acc);
    if (!growID.empty()) {
        std::string pass = kv["tankIDPass"];
        if (exists && !acc.passwordHash.empty() && acc.passwordHash != HashPassword(name, pass)) {
            SendAction(p.peer, "action|log\nmsg|`4Wrong password for " + name + ".``");
            SendAction(p.peer, "action|logon_fail\n");
            return;
        }
        if (!exists || acc.passwordHash.empty()) acc.passwordHash = HashPassword(name, pass);
    } else if (exists && !acc.passwordHash.empty()) {
        // A guest can't take a name that has a password; give them a free one.
        int n = 1;
        std::string base = name;
        Account other;
        do {
            name = base + "_" + std::to_string(++n);
        } while (LoadAccount(g_opt.dataDir, name, other) && !other.passwordHash.empty());
        exists = LoadAccount(g_opt.dataDir, name, acc);
    }
    if (IsServerBanned(name, p.ip)) {
        SendAction(p.peer, "action|log\nmsg|`4This server has banned you.``");
        SendAction(p.peer, "action|logon_fail\n");
        Log("refused banned %s (%s)", name.c_str(), p.ip.c_str());
        return;
    }
    for (auto& [peer, o] : g_players) {
        if (o.get() != &p && o->loggedIn && Lower(o->name) == Lower(name)) {
            SendAction(p.peer, "action|log\nmsg|`4" + name + " is already online.``");
            SendAction(p.peer, "action|logon_fail\n");
            return;
        }
    }

    p.name = name;
    p.passwordHash = acc.passwordHash;
    if (exists) {
        p.userID = acc.userID;
        p.inventory = acc.inventory;
        memcpy(p.clothes, acc.clothes, sizeof p.clothes);
        p.skin = acc.skin ? acc.skin : kDefaultSkin;
        p.gems = acc.gems;
        p.friends = acc.friends;
        p.recentWorlds = acc.recentWorlds;
        p.mutedUntil = acc.mutedUntil;
    } else {
        p.skin = kDefaultSkin;
        p.userID = NextUserID(g_opt.dataDir);
        GiveStarterKit(p);
    }
    p.loggedIn = true;
    Log("%s logged in (netID %d, userID %d%s)", p.name.c_str(), p.netID, p.userID, exists ? "" : ", new");
    Call(p.peer, VariantList("OnInitialLogonAccepted").u(ItemsDatHash()).str(g_opt.host).str("cache/"));
}

void SavePlayer(Player& p) {
    if (!p.loggedIn) return;
    Account acc;
    acc.userID = p.userID;
    acc.passwordHash = p.passwordHash;
    acc.inventory = p.inventory;
    memcpy(acc.clothes, p.clothes, sizeof acc.clothes);
    acc.skin = p.skin;
    acc.gems = p.gems;
    acc.name = p.name;
    acc.friends = p.friends;
    acc.recentWorlds = p.recentWorlds;
    acc.mutedUntil = p.mutedUntil;
    if (!SaveAccount(g_opt.dataDir, p.name, acc)) LogError("could not save %s", p.name.c_str());
}

void EnterGame(Player& p) {
    p.enteredGame = true;
    SendInventory(p);  // the inventory packet is what closes the client's connecting screen
    Call(p.peer, VariantList("OnSetBux").i(p.gems));
    Call(p.peer, VariantList("OnRequestWorldSelectMenu").str(""));
    ShowWorldList(p);
    Console(p, "`oWelcome back to `wNovember 2012``, `w" + p.name + "``. Type `w/help`` for commands.``");
    int online = static_cast<int>(g_players.size());
    Console(p, "`w" + std::to_string(online) + "`` " + (online == 1 ? "player is" : "players are") + " online.");
    OnLoggedIn(p);
}

// Text from message types 2 and 3. The client appends one junk byte.
void OnText(Player& p, std::string text) {
    size_t nul = text.find('\0');
    if (nul != std::string::npos) text.resize(nul);
    auto kv = ParseText(text);

    if (!p.loggedIn) {
        if (kv.count("requestedName") || kv.count("tankIDName")) Login(p, kv);
        return;
    }
    std::string action = kv["action"];
    if (action == "refresh_item_data") {
        TankPacket t;
        t.type = TANK_ITEMS_DAT;
        SendTank(p.peer, t, ItemsDat());
    } else if (action == "enter_game") {
        if (!p.enteredGame) EnterGame(p);
    } else if (action == "join_request") {
        JoinWorld(p, kv["name"]);
    } else if (action == "quit_to_exit") {
        LeaveWorld(p, true);
    } else if (action == "quit") {
        enet_peer_disconnect_later(p.peer, 0);
    } else if (action == "input") {
        size_t at = text.find("|text|");
        if (at != std::string::npos) Chat(p, text.substr(at + 6));
    } else if (action == "respawn") {
        Respawn(p);
    } else if (action == "setSkin") {
        p.skin = static_cast<uint32_t>(strtoul(kv["color"].c_str(), nullptr, 10));
        ForWorld(p.world, [&](Player& o) { Call(o.peer, ClothingCall(p, false), p.netID); });
    } else if (action == "drop" || action == "info") {
        size_t at = text.find("|itemID|");
        int id = at == std::string::npos ? -1 : atoi(text.c_str() + at + 8);
        if (action == "drop") DropDialog(p, id);
        else ItemInfo(p, id);
    } else if (action == "dialog_return") {
        DialogReturn(p, kv);
    } else if (action == "growid") {
        Console(p, "To keep your name, go to Options, tick GrowID and set a name and password, then reconnect.");
    } else {
        Log("%s sent unhandled text: %s", p.name.c_str(), text.c_str());
    }
}

// ---------------------------------------------------------------- entry points

int Player::Count(uint16_t id) const {
    for (auto& it : inventory)
        if (it.id == id) return it.count;
    return 0;
}

bool Player::Add(uint16_t id, int count) {
    for (auto& it : inventory) {
        if (it.id != id) continue;
        if (it.count + count > kMaxStack) return false;
        it.count = static_cast<uint8_t>(it.count + count);
        return true;
    }
    if (inventory.size() >= kMaxSlots || count > kMaxStack) return false;
    inventory.push_back({id, static_cast<uint8_t>(count)});
    return true;
}

void Player::Remove(uint16_t id, int count) {
    for (size_t i = 0; i < inventory.size(); i++) {
        if (inventory[i].id != id) continue;
        if (inventory[i].count <= count) {
            inventory.erase(inventory.begin() + i);
            const ItemDef* def = GetItem(id);
            if (def && IsClothes(*def) && clothes[def->bodyPart] == id) clothes[def->bodyPart] = 0;
        } else {
            inventory[i].count = static_cast<uint8_t>(inventory[i].count - count);
        }
        return;
    }
}

bool Player::Wearing(uint16_t id) const {
    for (uint16_t c : clothes)
        if (c == id && id != 0) return true;
    return false;
}


void OnConnect(ENetPeer* peer) {
    auto p = std::make_unique<Player>();
    p->peer = peer;
    p->netID = g_nextNetID++;
    char ip[64] = {};
    if (enet_address_get_host_ip(&peer->address, ip, sizeof ip) == 0) p->ip = ip;
    g_players[peer] = std::move(p);
    uint32_t zero = 0;
    Send(peer, MSG_SERVER_HELLO, &zero, 4);
}

void OnReceive(ENetPeer* peer, const uint8_t* data, size_t len) {
    auto it = g_players.find(peer);
    if (it == g_players.end() || len < 4) return;
    Player& p = *it->second;
    uint32_t type;
    memcpy(&type, data, 4);
    if (type == MSG_GENERIC_TEXT || type == MSG_GAME_MESSAGE) {
        OnText(p, std::string(reinterpret_cast<const char*>(data + 4), len - 4 - (len > 4 ? 1 : 0)));
        return;
    }
    if (type != MSG_GAME_PACKET || len < 4 + sizeof(TankPacket) || !p.loggedIn) return;
    TankPacket t;
    memcpy(&t, data + 4, sizeof t);
    switch (t.type) {
    case TANK_STATE: OnState(p, data + 4); break;
    case TANK_TILE_CHANGE: OnTileChange(p, t); break;
    case TANK_TILE_ACTIVATE: OnTileActivate(p, t); break;
    case TANK_ITEM_ACTIVATE: OnItemActivate(p, t); break;
    case TANK_OBJECT_PICKUP: PickUp(p, static_cast<uint32_t>(t.intData)); break;
    default: Log("%s sent tank type %d", p.name.c_str(), t.type); break;
    }
}

void OnDisconnect(ENetPeer* peer) {
    auto it = g_players.find(peer);
    if (it == g_players.end()) return;
    Player& p = *it->second;
    if (p.loggedIn) Log("%s disconnected", p.name.c_str());
    if (p.loggedIn) {
        CancelTrade(p, "disconnected");
        OnLoggedOut(p);
    }
    LeaveWorld(p, false);
    SavePlayer(p);
    g_players.erase(it);
}

void Tick() {
    static uint32_t lastSave = NowMs();
    if (NowMs() - lastSave < 60000) return;
    lastSave = NowMs();
    SaveEverything();
}

void SaveEverything() {
    for (auto& [name, w] : g_worlds) SaveWorldIfDirty(*w);
    for (auto& [peer, p] : g_players) SavePlayer(*p);
    // Drop worlds nobody is in.
    for (auto it = g_worlds.begin(); it != g_worlds.end();) {
        if (PlayersIn(it->second.get()) == 0 && !it->second->dirty) it = g_worlds.erase(it);
        else ++it;
    }
}

void ConsoleCommand(const std::string& line) {
    if (ServerConsoleCommand(line)) return;
    if (line == "save") {
        SaveEverything();
        Log("saved");
    } else if (line == "players") {
        Log("%zu online", g_players.size());
        for (auto& [peer, p] : g_players)
            Log("  %s (netID %d) in %s", p->name.c_str(), p->netID, p->world ? p->world->name.c_str() : "menu");
    } else if (line.rfind("say ", 0) == 0) {
        for (auto& [peer, p] : g_players)
            if (p->enteredGame) Console(*p, "`4Server:`` " + line.substr(4));
    } else {
        Log("commands: players, say <text>, save, stop, kick <name>, ban <name>, unban <name>, mod <name>, "
            "unmod <name>, mute <name> <minutes>, unmute <name>");
    }
}
