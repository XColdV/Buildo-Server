// Test bot: logs in like Buildo does, joins a world, walks back and forth and
// chats, and prints every function call the server sends it.
//   bot.exe [name] [world] [seconds] [port] [script]
// Without a script it walks back and forth and chats. A script is steps split
// by ';', one every half second after it spawns:
//   /text            chat (or a command)
//   !a|b\nc|d        raw text message, \n for line breaks (e.g. a dialog_return)
//   @place id dx dy  place an item dx,dy tiles from where it spawned
//   @punch dx dy     punch that tile
//   @wait ms
#include <enet/enet.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/proto.h"

namespace {

ENetPeer* g_peer;

void Send(uint32_t type, const void* data, size_t len) {
    ENetPacket* p = enet_packet_create(nullptr, 4 + len + 1, ENET_PACKET_FLAG_RELIABLE);
    memcpy(p->data, &type, 4);
    memcpy(p->data + 4, data, len);
    p->data[4 + len] = 0;
    enet_peer_send(g_peer, 0, p);
}

void Text(uint32_t type, const std::string& s) { Send(type, s.data(), s.size()); }

std::string Describe(const uint8_t* ext, size_t n) {
    std::string out;
    if (n < 1) return out;
    size_t pos = 1;
    int count = ext[0];
    for (int i = 0; i < count && pos + 2 <= n; i++) {
        uint8_t type = ext[pos + 1];
        pos += 2;
        if (i) out += " | ";
        if (type == 2) {
            uint32_t len;
            memcpy(&len, ext + pos, 4);
            std::string s(reinterpret_cast<const char*>(ext + pos + 4), len);
            for (auto& c : s)
                if (c == '\n') c = ';';
            out += s;
            pos += 4 + len;
        } else if (type == 1 || type == 5 || type == 9) {
            uint32_t v;
            memcpy(&v, ext + pos, 4);
            out += std::to_string(v);
            pos += 4;
        } else if (type == 3) {
            float v[2];
            memcpy(v, ext + pos, 8);
            out += std::to_string(v[0]) + "," + std::to_string(v[1]);
            pos += 8;
        } else if (type == 4) {
            float v[3];
            memcpy(v, ext + pos, 12);
            out += std::to_string(v[0]) + "," + std::to_string(v[1]) + "," + std::to_string(v[2]);
            pos += 12;
        } else {
            out += "?";
            break;
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string name = argc > 1 ? argv[1] : "Seth";
    std::string world = argc > 2 ? argv[2] : "START";
    int seconds = argc > 3 ? atoi(argv[3]) : 60;
    int port = argc > 4 ? atoi(argv[4]) : 17095;
    std::vector<std::string> script;
    if (argc > 5) {
        std::string all = argv[5], step;
        for (char c : all + ";") {
            if (c == ';') {
                if (!step.empty()) script.push_back(step);
                step.clear();
            } else {
                step += c;
            }
        }
    }

    enet_initialize();
    ENetHost* host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, nullptr, 1, 2, 0, 0);
    host->usingNewPacket = 0;
    host->usingNewPacketForServer = 0;
    host->checksum = enet_crc32;
    enet_host_compress_with_range_coder(host);
    ENetAddress addr;
    enet_address_set_host(&addr, ENET_ADDRESS_TYPE_IPV4, "127.0.0.1");
    addr.port = static_cast<enet_uint16>(port);
    g_peer = enet_host_connect(host, &addr, 2, 0);

    float x = 0, y = 0, startX = 0;
    bool spawned = false;
    int netID = 0;
    uint32_t start = enet_time_get(), lastMove = 0, lastChat = 0;
    int dir = 1;
    float spawnX = 0, spawnY = 0;
    size_t nextStep = 0;
    uint32_t nextStepAt = 0;
    ENetEvent ev;
    while (enet_time_get() - start < static_cast<uint32_t>(seconds) * 1000) {
        while (enet_host_service(host, &ev, 20) > 0) {
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                printf("disconnected\n");
                return 1;
            }
            if (ev.type != ENET_EVENT_TYPE_RECEIVE) continue;
            const uint8_t* d = ev.packet->data;
            size_t n = ev.packet->dataLength;
            uint32_t type;
            memcpy(&type, d, 4);
            if (type == 1) {
                Text(2, "requestedName|" + name + "\nprotocol|1\ngame_version|0.01\nhash|1\nhash2|1\nplatformID|0\n"
                        "deviceVersion|0\ncountry|nl\n");
            } else if (type == 3 && n > 5) {
                printf("text: %.*s\n", static_cast<int>(n - 5), d + 4);
            } else if (type == 4 && n >= 4 + sizeof(TankPacket)) {
                TankPacket t;
                memcpy(&t, d + 4, sizeof t);
                const uint8_t* ext = d + 4 + sizeof t;
                if (t.type == 1) {
                    std::string call = Describe(ext, t.extSize);
                    printf("call net=%d: %s\n", t.netID, call.c_str());
                    if (call.rfind("OnInitialLogonAccepted", 0) == 0) Text(2, "action|enter_game\n");
                    if (call.rfind("OnRequestWorldSelectMenu", 0) == 0) Text(3, "action|join_request\nname|" + world + "\n");
                    if (call.rfind("OnSpawn", 0) == 0 && call.find("type|local") != std::string::npos) {
                        sscanf(strstr(call.c_str(), "posXY|") + 6, "%f|%f", &x, &y);
                        sscanf(strstr(call.c_str(), "netID|") + 6, "%d", &netID);
                        startX = x;
                        spawnX = x;
                        spawnY = y;
                        spawned = true;
                    }
                } else if (t.type != 0) {
                    printf("tank %d net=%d item=%d int=%d tile=%d,%d ext=%u", t.type, t.netID, t.item, t.intData,
                           t.tileX, t.tileY, t.extSize);
                    if (t.type == 15)
                        for (uint32_t i = 0; i + 1 < t.extSize && i < 24; i += 2) printf(" %u", ext[i] | (ext[i + 1] << 8));
                    printf("\n");
                }
            }
            enet_packet_destroy(ev.packet);
        }
        uint32_t now = enet_time_get();
        if (spawned && nextStep < script.size() && now >= nextStepAt) {
            std::string step = script[nextStep++];
            nextStepAt = now + 500;
            printf("> %s\n", step.c_str());
            if (step[0] == '/') {
                Text(2, "action|input\n|text|" + step);
            } else if (step[0] == '!') {
                std::string raw;
                for (size_t i = 1; i < step.size(); i++) {
                    if (step[i] == '\\' && i + 1 < step.size() && step[i + 1] == 'n') {
                        raw += '\n';
                        i++;
                    } else {
                        raw += step[i];
                    }
                }
                Text(2, raw);
            } else if (step.rfind("@wait", 0) == 0) {
                nextStepAt = now + atoi(step.c_str() + 5);
            } else if (step.rfind("@place", 0) == 0 || step.rfind("@punch", 0) == 0) {
                int id = 18, dx = 0, dy = 0;
                if (step[1] == 'p' && step[2] == 'l') sscanf(step.c_str() + 6, "%d %d %d", &id, &dx, &dy);
                else sscanf(step.c_str() + 6, "%d %d", &dx, &dy);
                TankPacket t;
                t.type = 3;
                t.intData = id;
                t.tileX = static_cast<int>(spawnX / 32) + dx;
                t.tileY = static_cast<int>(spawnY / 32) + dy;
                Send(4, &t, sizeof t);
            }
        }
        if (spawned && script.empty() && now - lastMove > 200) {
            lastMove = now;
            x += dir * 16.f;
            if (x > startX + 96 || x < startX - 96) dir = -dir;
            TankPacket t;
            t.type = 0;
            t.posX = x;
            t.posY = y;
            t.speedX = dir * 80.f;
            t.flags = dir < 0 ? 0x10 : 0;
            t.tileX = t.tileY = -1;
            Send(4, &t, sizeof t);
        }
        if (spawned && script.empty() && now - lastChat > 8000) {
            lastChat = now;
            Text(2, "action|input\n|text|hello from the bot, netID " + std::to_string(netID));
        }
    }
    enet_peer_disconnect(g_peer, 0);
    enet_host_service(host, &ev, 200);
    return 0;
}
