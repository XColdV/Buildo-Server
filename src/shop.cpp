// /shop: spend gems on what extra_items.txt lists with add_shop / add_shop_pack.
#include <algorithm>
#include <sstream>

#include "items.h"
#include "log.h"
#include "server.h"

bool CanTake(const Player& p, const std::map<uint16_t, int>& items) {
    size_t slots = p.inventory.size();
    for (auto& [id, n] : items) {
        int have = p.Count(id);
        if (have + n > kMaxStack) return false;
        if (have == 0) slots++;
    }
    return slots <= kMaxSlots;
}

namespace {

std::string Describe(const ShopEntry& e) {
    std::string name = e.item ? GetItem(e.item)->name : e.name;
    if (e.count > 1) name += e.item ? " x" + std::to_string(e.count) : " (" + std::to_string(e.count) + " seeds)";
    return name;
}

void ShowShop(Player& p) {
    std::string d = "set_default_color|`o\nadd_label|big|`wShop``|left|\n";
    d += "add_textbox|You have `w" + std::to_string(p.gems) + "`` gems. Gems drop from blocks you break.|left|\n";
    const auto& entries = ShopEntries();
    for (size_t i = 0; i < entries.size(); i++) {
        const ShopEntry& e = entries[i];
        d += "add_button|buy_" + std::to_string(i) + "|" + Describe(e) + " - " + std::to_string(e.price) + " gems|\n";
    }
    if (entries.empty()) d += "add_textbox|Nothing for sale. The server's extra_items.txt has no add_shop lines.|left|\n";
    d += "end_dialog|shop|Close||\n";
    Dialog(p, d);
}

void Buy(Player& p, size_t index) {
    const auto& entries = ShopEntries();
    if (index >= entries.size()) return;
    const ShopEntry& e = entries[index];
    if (p.gems < e.price) {
        Console(p, "`4You need `w" + std::to_string(e.price - p.gems) + "`` more gems for that.``");
        return;
    }
    std::map<uint16_t, int> items;
    if (e.item) items[e.item] = e.count;
    else
        for (int i = 0; i < e.count; i++) items[e.choices[Rand(0, static_cast<int>(e.choices.size()) - 1)]]++;
    if (!CanTake(p, items)) {
        Console(p, "`4That won't fit in your backpack.``");
        return;
    }
    p.gems -= e.price;
    std::string got;
    for (auto& [id, n] : items) {
        p.Add(id, n);
        got += (got.empty() ? "" : ", ") + std::to_string(n) + " " + GetItem(id)->name;
    }
    SendInventory(p);
    SendGems(p);
    SendAction(p.peer, "action|play_sfx\nfile|audio/object_spawn.wav\ndelayMS|0\n");
    Console(p, "`2You bought " + Describe(e) + ": `w" + got + "``.``");
    Log("%s bought %s for %d gems", p.name.c_str(), Describe(e).c_str(), e.price);
}

}  // namespace

bool ShopCommand(Player& p, const std::string& cmd, std::istringstream&) {
    if (cmd != "/shop" && cmd != "/store") return false;
    ShowShop(p);
    return true;
}

bool ShopDialog(Player& p, KeyValues& kv) {
    if (kv["dialog_name"] != "shop") return false;
    const std::string& button = kv["buttonClicked"];
    if (button.rfind("buy_", 0) == 0) {
        Buy(p, static_cast<size_t>(atoi(button.c_str() + 4)));
        ShowShop(p);
    }
    return true;
}
