// World list, friends, private messages, moderation and chat limits.
#include <algorithm>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

#include "accounts.h"
#include "items.h"
#include "log.h"
#include "server.h"

namespace {

constexpr size_t kRecentWorlds = 8;
constexpr size_t kMaxFriends = 50;

struct BanEntry {
    std::string name;  // lower case
    std::string ip;    // may be empty
};

std::set<std::string> g_mods;  // lower-case names
std::vector<BanEntry> g_bans;
bool g_listsLoaded = false;

std::string ModsPath() { return g_opt.dataDir + "/mods.txt"; }
std::string BansPath() { return g_opt.dataDir + "/bans.txt"; }

// data/mods.txt: one name per line. data/bans.txt: "name|ip" per line.
void LoadLists() {
    if (g_listsLoaded) return;
    g_listsLoaded = true;
    std::ifstream mods(ModsPath());
    std::string line;
    while (std::getline(mods, line))
        if (!line.empty()) g_mods.insert(Lower(line));
    std::ifstream bans(BansPath());
    while (std::getline(bans, line)) {
        if (line.empty()) continue;
        size_t bar = line.find('|');
        g_bans.push_back({Lower(line.substr(0, bar)), bar == std::string::npos ? "" : line.substr(bar + 1)});
    }
}

void SaveLists() {
    std::ofstream mods(ModsPath(), std::ios::trunc);
    for (auto& m : g_mods) mods << m << "\n";
    std::ofstream bans(BansPath(), std::ios::trunc);
    for (auto& b : g_bans) bans << b.name << "|" << b.ip << "\n";
}

int64_t Now() { return static_cast<int64_t>(time(nullptr)); }

std::string Rest(std::istringstream& in) {
    std::string s;
    std::getline(in, s);
    s.erase(0, s.find_first_not_of(' '));
    return s;
}

std::string MinutesText(int64_t seconds) {
    int64_t m = (seconds + 59) / 60;
    return std::to_string(m) + (m == 1 ? " minute" : " minutes");
}

void SetMute(const std::string& rawName, int minutes, std::string& reply) {
    int64_t until = minutes > 0 ? Now() + static_cast<int64_t>(minutes) * 60 : 0;
    if (Player* o = OnlineByName(rawName)) {
        o->mutedUntil = until;
        SavePlayer(*o);
        Console(*o, until ? "`4You've been muted for " + MinutesText(until - Now()) + ".``" : "`2You can talk again.``");
        reply = (until ? "Muted " : "Unmuted ") + o->name + ".";
        return;
    }
    Account acc;
    if (!LoadAccount(g_opt.dataDir, rawName, acc)) {
        reply = "Nobody called " + rawName + " has played here.";
        return;
    }
    acc.mutedUntil = until;
    SaveAccount(g_opt.dataDir, acc.name.empty() ? rawName : acc.name, acc);
    reply = (until ? "Muted " : "Unmuted ") + rawName + ".";
}

void ServerBan(const std::string& rawName, std::string& reply) {
    LoadLists();
    std::string ip;
    Player* o = OnlineByName(rawName);
    if (o) ip = o->ip;
    g_bans.push_back({Lower(rawName), ip});
    SaveLists();
    if (o) {
        SendAction(o->peer, "action|log\nmsg|`4This server has banned you.``");
        enet_peer_disconnect_later(o->peer, 0);
    }
    reply = "Banned " + rawName + (ip.empty() ? "." : " (and " + ip + ").");
}

void ServerUnban(const std::string& rawName, std::string& reply) {
    LoadLists();
    std::string want = Lower(rawName);
    size_t before = g_bans.size();
    g_bans.erase(std::remove_if(g_bans.begin(), g_bans.end(), [&](const BanEntry& b) { return b.name == want; }),
                 g_bans.end());
    SaveLists();
    reply = before == g_bans.size() ? rawName + " wasn't banned." : "Unbanned " + rawName + ".";
}

bool Kick(Player& by, Player& target, bool anywhere) {
    if (!target.world || (!anywhere && target.world != by.world)) return false;
    World* w = target.world;
    LeaveWorld(target, true);
    Console(target, "`4" + by.name + " kicked you out of `w" + w->name + "``.``");
    ForWorld(w, [&](Player& o) { Console(o, "`5" + target.name + " was kicked by " + by.name + ".``"); });
    return true;
}

}  // namespace

// ---------------------------------------------------------------- world list

void RememberWorld(Player& p, const std::string& world) {
    auto& r = p.recentWorlds;
    r.erase(std::remove(r.begin(), r.end(), world), r.end());
    r.insert(r.begin(), world);
    if (r.size() > kRecentWorlds) r.resize(kRecentWorlds);
}

// The 2012 world menu is only a name box (0x42f220), so the list comes as a
// dialog on top of it.
void ShowWorldList(Player& p) {
    std::vector<std::pair<int, std::string>> active;
    for (auto& [name, w] : g_worlds) {
        int n = PlayersIn(w.get());
        if (n > 0) active.push_back({n, name});
    }
    std::sort(active.begin(), active.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    if (active.size() > 8) active.resize(8);

    std::string d = "set_default_color|`o\nadd_label|big|`wWorlds``|left|\n";
    d += "add_textbox|Tap a world to go there, or close this and type a name.|left|\n";
    if (!active.empty()) {
        d += "add_label|small|`wActive now``|left|\n";
        for (auto& [n, name] : active)
            d += "add_button|w_" + name + "|" + name + " (" + std::to_string(n) + (n == 1 ? " player)" : " players)") + "|\n";
    }
    std::vector<std::string> recent;
    for (auto& r : p.recentWorlds)
        if (std::none_of(active.begin(), active.end(), [&](auto& a) { return a.second == r; })) recent.push_back(r);
    if (!recent.empty()) {
        d += "add_label|small|`wYour recent worlds``|left|\n";
        for (auto& r : recent) d += "add_button|w_" + r + "|" + r + "|\n";
    }
    if (active.empty() && recent.empty()) d += "add_button|w_START|START|\n";
    d += "end_dialog|world_list|Close||\n";
    Dialog(p, d);
}

// ---------------------------------------------------------------- permissions

bool IsMod(const Player& p) {
    LoadLists();
    return g_mods.count(Lower(p.name)) > 0;
}

// The World Lock's owner, or without one, anyone who owns a lock in the world.
bool IsWorldOwner(const Player& p, const World& w) {
    if (IsMod(p)) return true;
    if (w.worldLock >= 0) return w.tiles[w.worldLock].owner == p.userID;
    for (const Tile& t : w.tiles) {
        const ItemDef* def = GetItem(t.fg);
        if (def && IsLock(*def) && t.owner == p.userID) return true;
    }
    return false;
}

bool IsServerBanned(const std::string& name, const std::string& ip) {
    LoadLists();
    std::string want = Lower(name);
    for (auto& b : g_bans)
        if (b.name == want || (!b.ip.empty() && b.ip == ip)) return true;
    return false;
}

bool IsWorldBanned(const Player& p, const World& w) {
    if (IsMod(p)) return false;
    return std::any_of(w.bans.begin(), w.bans.end(), [&](auto& b) { return b.first == p.userID; });
}

bool ChatAllowed(Player& p, const std::string& msg) {
    int64_t now = Now();
    if (p.mutedUntil > now) {
        Console(p, "`4You're muted for " + MinutesText(p.mutedUntil - now) + ".``");
        return false;
    }
    uint32_t ms = NowMs();
    if (msg == p.lastChat && ms - p.lastChatMs < 4000) return false;
    p.chatTimes.erase(std::remove_if(p.chatTimes.begin(), p.chatTimes.end(), [&](uint32_t t) { return ms - t > 6000; }),
                      p.chatTimes.end());
    if (p.chatTimes.size() >= 5) {
        p.mutedUntil = now + 60;
        p.chatTimes.clear();
        Console(p, "`4Slow down. You're muted for a minute.``");
        return false;
    }
    p.chatTimes.push_back(ms);
    p.lastChat = msg;
    p.lastChatMs = ms;
    return true;
}

// ---------------------------------------------------------------- friends

namespace {
bool HasFriend(const Player& p, const std::string& name) {
    std::string want = Lower(name);
    return std::any_of(p.friends.begin(), p.friends.end(), [&](auto& f) { return Lower(f) == want; });
}
}  // namespace

void OnLoggedIn(Player& p) {
    int online = 0;
    for (auto& [peer, o] : g_players) {
        if (o.get() == &p || !o->loggedIn) continue;
        if (HasFriend(*o, p.name)) Console(*o, "`3[`w" + p.name + "`` is online.]``");
        if (HasFriend(p, o->name)) online++;
    }
    if (online) Console(p, "`3" + std::to_string(online) + (online == 1 ? " friend is" : " friends are") + " online. /friends``");
}

void OnLoggedOut(Player& p) {
    for (auto& [peer, o] : g_players)
        if (o.get() != &p && o->loggedIn && HasFriend(*o, p.name)) Console(*o, "`3[`w" + p.name + "`` went offline.]``");
}

// ---------------------------------------------------------------- commands

bool SocialCommand(Player& p, const std::string& cmd, std::istringstream& in) {
    if (cmd == "/worlds") {
        ShowWorldList(p);
    } else if (cmd == "/msg" || cmd == "/r") {
        std::string to;
        if (cmd == "/msg") in >> to;
        else to = p.lastWhisperFrom;
        std::string text = Rest(in);
        Player* o = to.empty() ? nullptr : OnlineByName(to);
        if (text.empty()) {
            Console(p, cmd == "/msg" ? "Use: /msg <name> <text>" : "Use: /r <text>");
        } else if (!o) {
            Console(p, (to.empty() ? std::string("Nobody") : "`w" + to + "``") + " isn't online.");
        } else if (p.mutedUntil > Now()) {
            Console(p, "`4You're muted for " + MinutesText(p.mutedUntil - Now()) + ".``");
        } else {
            Console(*o, "`c[MSG from `w" + p.name + "``] " + text + "``");
            Console(p, "`c[MSG to `w" + o->name + "``] " + text + "``");
            o->lastWhisperFrom = p.name;
        }
    } else if (cmd == "/friends") {
        if (p.friends.empty()) {
            Console(p, "No friends yet. Add someone with /addfriend <name>.");
            return true;
        }
        std::string line;
        for (auto& f : p.friends) {
            Player* o = OnlineByName(f);
            line += (line.empty() ? "" : ", ") + std::string(o ? "`2" : "`8") + f +
                    (o ? (o->world ? " (" + o->world->name + ")" : " (menu)") : "") + "``";
        }
        Console(p, "`oFriends: " + line + "``");
    } else if (cmd == "/addfriend") {
        std::string name;
        in >> name;
        int id;
        std::string real;
        if (name.empty() || !FindPlayer(name, id, real)) {
            Console(p, "Nobody called `w" + name + "`` has played here.");
        } else if (id == p.userID) {
            Console(p, "That's you.");
        } else if (HasFriend(p, real)) {
            Console(p, "`w" + real + "`` is already your friend.");
        } else if (p.friends.size() >= kMaxFriends) {
            Console(p, "Your friend list is full.");
        } else {
            p.friends.push_back(real);
            Console(p, "`3Added `w" + real + "`` as a friend.``");
            if (Player* o = OnlineByName(real))
                if (!HasFriend(*o, p.name)) Console(*o, "`3" + p.name + " added you as a friend. /addfriend " + p.name + " to add them back.``");
        }
    } else if (cmd == "/unfriend") {
        std::string name = Lower(Rest(in));
        size_t before = p.friends.size();
        p.friends.erase(std::remove_if(p.friends.begin(), p.friends.end(), [&](auto& f) { return Lower(f) == name; }),
                        p.friends.end());
        Console(p, before == p.friends.size() ? "They weren't on your list." : "Removed.");
    } else if (cmd == "/kick" || cmd == "/ban" || cmd == "/unban" || cmd == "/bans") {
        World* w = p.world;
        if (!w || !IsWorldOwner(p, *w)) {
            Console(p, "Only the world's owner can do that.");
            return true;
        }
        std::string name = Rest(in);
        if (cmd == "/bans") {
            std::string list;
            for (auto& b : w->bans) list += (list.empty() ? "" : ", ") + b.second;
            Console(p, list.empty() ? "Nobody is banned from this world." : "`oBanned here: `w" + list + "``");
            return true;
        }
        if (cmd == "/unban") {
            std::string want = Lower(name);
            size_t before = w->bans.size();
            w->bans.erase(std::remove_if(w->bans.begin(), w->bans.end(), [&](auto& b) { return Lower(b.second) == want; }),
                          w->bans.end());
            w->dirty = true;
            Console(p, before == w->bans.size() ? name + " wasn't banned here." : "Unbanned " + name + ".");
            return true;
        }
        Player* o = OnlineByName(name);
        if (cmd == "/kick") {
            if (o && IsMod(*o) && !IsMod(p)) Console(p, "You can't kick a mod.");
            else if (!o || o == &p || !Kick(p, *o, IsMod(p))) Console(p, "`w" + name + "`` isn't in this world.");
            return true;
        }
        int id;
        std::string real;
        if (!FindPlayer(name, id, real) || id == p.userID) {
            Console(p, "Nobody called `w" + name + "`` has played here.");
            return true;
        }
        if (std::none_of(w->bans.begin(), w->bans.end(), [&](auto& b) { return b.first == id; })) w->bans.push_back({id, real});
        w->dirty = true;
        ForWorld(w, [&](Player& x) { Console(x, "`5" + real + " is banned from `w" + w->name + "``.``"); });
        if (o && o->world == w) LeaveWorld(*o, true);
    } else if (cmd == "/mute" || cmd == "/unmute" || cmd == "/sban") {
        if (!IsMod(p)) return false;
        std::string name, reply;
        int minutes = 10;
        in >> name >> minutes;
        if (cmd == "/sban") ServerBan(name, reply);
        else SetMute(name, cmd == "/mute" ? std::max(minutes, 1) : 0, reply);
        Console(p, reply);
        Log("%s: %s", p.name.c_str(), reply.c_str());
    } else {
        return false;
    }
    return true;
}

bool SocialDialog(Player& p, KeyValues& kv) {
    if (kv["dialog_name"] != "world_list") return false;
    const std::string& button = kv["buttonClicked"];
    if (button.rfind("w_", 0) == 0) JoinWorld(p, button.substr(2));
    return true;
}

// ---------------------------------------------------------------- server window

bool ServerConsoleCommand(const std::string& line) {
    std::istringstream in(line);
    std::string cmd, name, reply;
    in >> cmd >> name;
    if (cmd == "kick" && !name.empty()) {
        Player* o = OnlineByName(name);
        if (!o) reply = name + " isn't online.";
        else {
            SendAction(o->peer, "action|log\nmsg|`4The server kicked you.``");
            enet_peer_disconnect_later(o->peer, 0);
            reply = "Kicked " + o->name + ".";
        }
    } else if (cmd == "ban" && !name.empty()) {
        ServerBan(name, reply);
    } else if (cmd == "unban" && !name.empty()) {
        ServerUnban(name, reply);
    } else if ((cmd == "mod" || cmd == "unmod") && !name.empty()) {
        LoadLists();
        if (cmd == "mod") g_mods.insert(Lower(name));
        else g_mods.erase(Lower(name));
        SaveLists();
        reply = name + (cmd == "mod" ? " is a mod now." : " is no longer a mod.");
        if (Player* o = OnlineByName(name)) Console(*o, cmd == "mod" ? "`2You're a mod now. /help shows the extra commands.``" : "You're no longer a mod.");
    } else if ((cmd == "mute" || cmd == "unmute") && !name.empty()) {
        int minutes = 10;
        in >> minutes;
        SetMute(name, cmd == "mute" ? std::max(minutes, 1) : 0, reply);
    } else {
        return false;
    }
    Log("%s", reply.c_str());
    return true;
}
