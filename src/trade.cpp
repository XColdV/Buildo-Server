// Trading: /trade <name> asks, the same command back accepts, then both sides
// fill an offer in a dialog and tick "accept". Any change to an offer clears
// both ticks. Both players have to stay in the same world.
#include <algorithm>
#include <memory>
#include <sstream>

#include "items.h"
#include "log.h"
#include "server.h"

namespace {

struct Trade {
    int user[2] = {0, 0};
    std::map<uint16_t, int> offer[2];
    bool accepted[2] = {false, false};
};

std::vector<std::unique_ptr<Trade>> g_trades;
std::map<int, std::pair<int, uint32_t>> g_requests;  // asker userID -> (asked userID, time)
constexpr uint32_t kRequestMs = 60000;

Trade* TradeOf(const Player& p, int* side = nullptr) {
    for (auto& t : g_trades)
        for (int s = 0; s < 2; s++)
            if (t->user[s] == p.userID) {
                if (side) *side = s;
                return t.get();
            }
    return nullptr;
}

std::string OfferText(const std::map<uint16_t, int>& offer) {
    std::string s;
    for (auto& [id, n] : offer) s += (s.empty() ? "" : ", ") + std::to_string(n) + " " + GetItem(id)->name;
    return s.empty() ? "nothing" : s;
}

// An item the player has, by ID or by (part of) its name.
uint16_t FindOwnItem(const Player& p, const std::string& what) {
    if (what.empty()) return 0;
    if (std::all_of(what.begin(), what.end(), ::isdigit)) {
        int id = atoi(what.c_str());
        return p.Count(static_cast<uint16_t>(id)) > 0 ? static_cast<uint16_t>(id) : 0;
    }
    std::string want = Lower(what);
    uint16_t partial = 0;
    for (const InvItem& it : p.inventory) {
        std::string name = Lower(GetItem(it.id)->name);
        if (name == want) return it.id;
        if (!partial && name.find(want) != std::string::npos) partial = it.id;
    }
    return partial;
}

bool Tradeable(uint16_t id) {
    const ItemDef* def = GetItem(id);
    return def && def->maxCanHold != 0 && id != ITEM_GEMS;
}

void ShowTrade(Player& p) {
    int side;
    Trade* t = TradeOf(p, &side);
    if (!t) return;
    Player* other = OnlineByUserID(t->user[1 - side]);
    if (!other) return;
    std::string d = "set_default_color|`o\nadd_label|big|`wTrading with " + other->name + "``|left|\n";
    d += "add_textbox|You give: `w" + OfferText(t->offer[side]) + "``|left|\n";
    d += "add_textbox|" + other->name + " gives: `w" + OfferText(t->offer[1 - side]) + "``|left|\n";
    d += "add_textbox|" + std::string(t->accepted[1 - side] ? "`2" + other->name + " has accepted.``" : other->name + " hasn't accepted yet.") + "|left|\n";
    d += "add_text_input|add_item|Add an item (name or ID)||24|\n";
    d += "add_text_input|add_count|How many|1|3|\n";
    d += "add_text_input|take_back|Take an item back (name or ID)||24|\n";
    d += "add_checkbox|accept|I accept this trade|" + std::string(t->accepted[side] ? "1" : "0") + "|\n";
    d += "add_button|cancel_trade|Cancel the trade|\n";
    d += "end_dialog|trade|Close|Update|\n";
    Dialog(p, d);
}

void EndTrade(Trade* t) {
    g_trades.erase(std::remove_if(g_trades.begin(), g_trades.end(), [&](auto& x) { return x.get() == t; }), g_trades.end());
}

void Finish(Trade* t) {
    Player* a = OnlineByUserID(t->user[0]);
    Player* b = OnlineByUserID(t->user[1]);
    if (!a || !b) return;
    for (int s = 0; s < 2; s++) {
        Player* from = s == 0 ? a : b;
        for (auto& [id, n] : t->offer[s])
            if (from->Count(id) < n) {
                Console(*a, "`4The trade failed: " + from->name + " doesn't have everything any more.``");
                Console(*b, "`4The trade failed: " + from->name + " doesn't have everything any more.``");
                t->accepted[0] = t->accepted[1] = false;
                return;
            }
    }
    if (!CanTake(*a, t->offer[1]) || !CanTake(*b, t->offer[0])) {
        Console(*a, "`4The trade failed: someone's backpack is too full.``");
        Console(*b, "`4The trade failed: someone's backpack is too full.``");
        t->accepted[0] = t->accepted[1] = false;
        return;
    }
    for (auto& [id, n] : t->offer[0]) a->Remove(id, n);
    for (auto& [id, n] : t->offer[1]) b->Remove(id, n);
    for (auto& [id, n] : t->offer[0]) b->Add(id, n);
    for (auto& [id, n] : t->offer[1]) a->Add(id, n);
    Log("trade: %s gave %s, %s gave %s", a->name.c_str(), OfferText(t->offer[0]).c_str(), b->name.c_str(),
        OfferText(t->offer[1]).c_str());
    for (Player* x : {a, b}) {
        SendInventory(*x);
        if (x->world) ForWorld(x->world, [&](Player& o) { Call(o.peer, ClothingCall(*x, false), x->netID); });
        SendAction(x->peer, "action|play_sfx\nfile|audio/object_collect.wav\ndelayMS|0\n");
        Console(*x, "`2Trade done.``");
        SavePlayer(*x);
    }
    EndTrade(t);
}

}  // namespace

void CancelTrade(Player& p, const std::string& why) {
    g_requests.erase(p.userID);
    int side;
    Trade* t = TradeOf(p, &side);
    if (!t) return;
    if (Player* other = OnlineByUserID(t->user[1 - side])) Console(*other, "`4The trade with " + p.name + " is off (" + why + ").``");
    EndTrade(t);
}

bool TradeCommand(Player& p, const std::string& cmd, std::istringstream& in) {
    if (cmd == "/canceltrade") {
        if (!TradeOf(p)) Console(p, "You aren't trading.");
        else {
            CancelTrade(p, "cancelled");
            Console(p, "Trade cancelled.");
        }
        return true;
    }
    if (cmd != "/trade") return false;
    std::string name;
    in >> name;
    if (name.empty()) {
        if (TradeOf(p)) ShowTrade(p);
        else Console(p, "Use: /trade <name>. They need to be in this world.");
        return true;
    }
    Player* o = OnlineByName(name);
    if (!o || o == &p || !p.world || o->world != p.world) {
        Console(p, "`w" + name + "`` has to be in this world to trade.");
        return true;
    }
    if (TradeOf(p) || TradeOf(*o)) {
        Console(p, TradeOf(p) ? "You're already trading. /trade shows it, /canceltrade ends it." : o->name + " is busy trading.");
        return true;
    }
    auto asked = g_requests.find(o->userID);
    if (asked != g_requests.end() && asked->second.first == p.userID && NowMs() - asked->second.second < kRequestMs) {
        g_requests.erase(asked);
        auto t = std::make_unique<Trade>();
        t->user[0] = o->userID;
        t->user[1] = p.userID;
        g_trades.push_back(std::move(t));
        Console(*o, "`2" + p.name + " accepted. /trade shows the trade.``");
        ShowTrade(p);
        ShowTrade(*o);
        return true;
    }
    g_requests[p.userID] = {o->userID, NowMs()};
    Console(*o, "`5" + p.name + " wants to trade. Type /trade " + p.name + " to start.``");
    Console(p, "Asked " + o->name + " to trade.");
    return true;
}

bool TradeDialog(Player& p, KeyValues& kv) {
    if (kv["dialog_name"] != "trade") return false;
    int side;
    Trade* t = TradeOf(p, &side);
    if (!t) return true;
    Player* other = OnlineByUserID(t->user[1 - side]);
    if (!other || other->world != p.world) {
        CancelTrade(p, "not in the same world");
        return true;
    }
    if (kv["buttonClicked"] == "cancel_trade") {
        CancelTrade(p, "cancelled");
        Console(p, "Trade cancelled.");
        return true;
    }

    bool changed = false;
    if (uint16_t id = FindOwnItem(p, kv["add_item"])) {
        int n = std::max(1, atoi(kv["add_count"].c_str()));
        if (!Tradeable(id)) {
            Console(p, "You can't trade that.");
        } else {
            int& offered = t->offer[side][id];
            offered = std::min(offered + n, p.Count(id));
            changed = true;
        }
    } else if (!kv["add_item"].empty()) {
        Console(p, "You don't have `w" + kv["add_item"] + "``.");
    }
    if (uint16_t id = FindOwnItem(p, kv["take_back"])) {
        if (t->offer[side].erase(id)) changed = true;
    }
    if (changed) {
        t->accepted[0] = t->accepted[1] = false;
        Console(*other, "`5" + p.name + " changed their offer. /trade to look.``");
    } else {
        bool accept = kv["accept"] == "1";
        if (accept && !t->accepted[side]) Console(*other, "`5" + p.name + " accepted the trade. /trade to look.``");
        t->accepted[side] = accept;
    }
    if (t->accepted[0] && t->accepted[1]) Finish(t);
    else ShowTrade(p);
    return true;
}
