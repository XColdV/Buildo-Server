// One text file per player in <data>/players, keyed by the lower-cased name.
#include "accounts.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

std::string PathFor(const std::string& dataDir, const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return dataDir + "/players/" + lower + ".txt";
}

}  // namespace

bool LoadAccount(const std::string& dataDir, const std::string& name, Account& out) {
    std::ifstream in(PathFor(dataDir, name));
    if (!in) return false;
    Account acc;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string key;
        std::getline(s, key, '|');
        if (key == "name") std::getline(s, acc.name);
        else if (key == "userID") s >> acc.userID;
        else if (key == "password") std::getline(s, acc.passwordHash);
        else if (key == "skin") s >> acc.skin;
        else if (key == "gems") s >> acc.gems;
        else if (key == "clothes") {
            for (auto& c : acc.clothes) {
                std::string v;
                std::getline(s, v, '|');
                c = static_cast<uint16_t>(atoi(v.c_str()));
            }
        } else if (key == "item") {
            std::string id, count;
            std::getline(s, id, '|');
            std::getline(s, count, '|');
            int c = atoi(count.c_str());
            if (c > 0) acc.inventory.push_back({static_cast<uint16_t>(atoi(id.c_str())), static_cast<uint8_t>(std::min(c, 255))});
        }
    }
    if (acc.userID <= 0) return false;
    out = acc;
    return true;
}

bool SaveAccount(const std::string& dataDir, const std::string& name, const Account& acc) {
    std::string path = PathFor(dataDir, name), tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << "name|" << name << "\nuserID|" << acc.userID << "\npassword|" << acc.passwordHash << "\nskin|"
            << acc.skin << "\ngems|" << acc.gems << "\nclothes";
        for (auto c : acc.clothes) out << "|" << c;
        out << "\n";
        for (const auto& it : acc.inventory) out << "item|" << it.id << "|" << static_cast<int>(it.count) << "\n";
        if (!out) return false;
    }
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

std::string HashPassword(const std::string& name, const std::string& password) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::string input = "buildo:" + lower + ":" + password;
    uint8_t digest[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(input.data()),
               static_cast<ULONG>(input.size()), digest, sizeof digest);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return std::string(hex, 64);
}

int NextUserID(const std::string& dataDir) {
    std::string path = dataDir + "/next_user_id.txt";
    int id = 1;
    {
        std::ifstream in(path);
        if (in) in >> id;
    }
    if (id < 1) id = 1;
    std::ofstream out(path, std::ios::trunc);
    out << (id + 1) << "\n";
    return id;
}
