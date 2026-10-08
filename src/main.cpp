// Buildo server: a small game server for the November 2012 Growtopia "V0.01"
// Windows build (Buildo.exe). See README.md for setup.
#include <enet/enet.h>
#include <windows.h>

#include <atomic>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "game.h"
#include "http.h"
#include "items.h"
#include "log.h"

namespace {

std::atomic<bool> g_stop{false};
std::mutex g_inputMutex;
std::deque<std::string> g_input;

// Lines typed into the server window, handed to the main loop.
void ReadStdin() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        g_input.push_back(line);
    }
}

void RunConsoleCommands() {
    std::deque<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        lines.swap(g_input);
    }
    for (const std::string& line : lines) {
        if (line == "stop" || line == "quit" || line == "exit") g_stop = true;
        else if (!line.empty()) ConsoleCommand(line);
    }
}

BOOL WINAPI OnConsoleEvent(DWORD) {
    g_stop = true;
    Sleep(3000);  // give the main loop time to save before Windows kills us
    return TRUE;
}

void Usage() {
    printf(
        "usage: buildo-server [options]\n"
        "  --port N        ENet (UDP) port, default 17095\n"
        "  --http-port N   server_data.php (TCP) port, default 80\n"
        "  --no-http       don't serve server_data.php\n"
        "  --host NAME     host name handed to the client, default localhost\n"
        "                  (Buildo never connects when given a bare IP)\n"
        "  --game DIR      Buildo folder, for game/item_definitions.txt\n"
        "                  (default: this folder, then the exe's folder)\n"
        "  --extra FILE    extra item definitions, default extra_items.txt\n"
        "  --data DIR      where worlds and players are saved, default ./data\n");
}

void ParseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                Usage();
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") g_opt.enetPort = atoi(next().c_str());
        else if (a == "--http-port") g_opt.httpPort = atoi(next().c_str());
        else if (a == "--host") g_opt.host = next();
        else if (a == "--no-http") g_opt.http = false;
        else if (a == "--game") g_opt.gameDir = next();
        else if (a == "--extra") g_opt.extraItems = next();
        else if (a == "--data") g_opt.dataDir = next();
        else {
            Usage();
            exit(a == "--help" || a == "-h" ? 0 : 2);
        }
    }
}

std::string ExeDir() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    return std::filesystem::path(std::string(path, n)).parent_path().string();
}

// The first of the candidate folders that has `relative` in it, or "".
std::string FindFile(const std::vector<std::string>& dirs, const std::string& relative) {
    for (const auto& d : dirs) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(d) / relative, ec)) return d;
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    ParseArgs(argc, argv);
    if (g_opt.gameDir.empty()) g_opt.gameDir = FindFile({".", ExeDir()}, "game/item_definitions.txt");
    if (g_opt.gameDir.empty()) {
        LogError("can't find game/item_definitions.txt. Put the server in the Buildo folder, or pass --game <folder>");
        return 1;
    }
    if (g_opt.extraItems.empty()) {
        std::string dir = FindFile({".", ExeDir()}, "extra_items.txt");
        if (!dir.empty()) g_opt.extraItems = dir + "/extra_items.txt";
    }
    if (!LoadItems(g_opt.gameDir + "/game/item_definitions.txt", g_opt.extraItems)) return 1;
    std::error_code ec;
    std::filesystem::create_directories(g_opt.dataDir + "/worlds", ec);
    std::filesystem::create_directories(g_opt.dataDir + "/players", ec);
    if (ec) {
        LogError("cannot create %s: %s", g_opt.dataDir.c_str(), ec.message().c_str());
        return 1;
    }

    if (enet_initialize() != 0) {
        LogError("enet_initialize failed");
        return 1;
    }
    if (g_opt.http) StartServerData(g_opt.httpPort, g_opt.host, g_opt.enetPort);

    ENetAddress address;
    enet_address_build_any(&address, ENET_ADDRESS_TYPE_IPV4);
    address.port = static_cast<enet_uint16>(g_opt.enetPort);
    ENetHost* host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, &address, 256, 2, 0, 0);
    if (!host) {
        LogError("cannot open UDP %d", g_opt.enetPort);
        return 1;
    }
    // 2012 ENet: classic header, CRC32 checksum, range coder. The struct is
    // malloc'd, so the new-header switches must be cleared by hand.
    host->usingNewPacket = 0;
    host->usingNewPacketForServer = 0;
    host->checksum = enet_crc32;
    enet_host_compress_with_range_coder(host);
    Log("ENet listening on UDP %d. Ctrl+C saves and quits.", g_opt.enetPort);
    SetConsoleCtrlHandler(OnConsoleEvent, TRUE);
    std::thread(ReadStdin).detach();
    Log("Commands: players, say <text>, save, stop");

    ENetEvent ev;
    while (!g_stop) {
        while (enet_host_service(host, &ev, 20) > 0) {
            switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                OnConnect(ev.peer);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                OnReceive(ev.peer, ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                OnDisconnect(ev.peer);
                break;
            default:
                break;
            }
        }
        Tick();
        RunConsoleCommands();
    }
    SaveEverything();
    Log("saved, bye");
    enet_host_destroy(host);
    enet_deinitialize();
    return 0;
}
