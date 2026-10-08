// server_data.php for Buildo. The client asks rtsoft.com:80 (hosts entry points it
// here) with a hand-written HTTP/1.0 request that uses bare "\n" line endings.
#include "http.h"
#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>

namespace {

std::string Body(const std::string& host, int port) {
    return "server|" + host + "\nport|" + std::to_string(port) +
           "\ntype|1\n#maint|Server is under maintenance.\nmeta|buildo\nRTENDMARKERBS1001\n";
}

void Serve(SOCKET client, std::string host, int enetPort) {
    DWORD timeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string raw;
    char chunk[2048];
    size_t bodyStart = std::string::npos;
    size_t contentLength = 0;
    for (;;) {
        int n = recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        raw.append(chunk, n);
        if (bodyStart == std::string::npos) {
            size_t a = raw.find("\n\n"), b = raw.find("\r\n\r\n");
            if (b != std::string::npos && (a == std::string::npos || b < a)) bodyStart = b + 4;
            else if (a != std::string::npos) bodyStart = a + 2;
            if (bodyStart != std::string::npos) {
                std::string head = raw.substr(0, bodyStart);
                for (auto& c : head) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                size_t cl = head.find("content-length:");
                if (cl != std::string::npos) contentLength = strtoul(head.c_str() + cl + 15, nullptr, 10);
            }
        }
        if (bodyStart != std::string::npos && raw.size() >= bodyStart + contentLength) break;
        if (raw.size() > 65536) break;
    }

    std::string firstLine = raw.substr(0, raw.find('\n'));
    if (!firstLine.empty() && firstLine.back() == '\r') firstLine.pop_back();
    Log("HTTP %s", firstLine.c_str());

    std::string status = "200 OK", body;
    if (firstLine.find("/growtopia/server_data.php") != std::string::npos) body = Body(host, enetPort);
    else status = "404 Not Found";

    std::string reply = "HTTP/1.0 " + status + "\r\nContent-Type: text/html\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    send(client, reply.data(), static_cast<int>(reply.size()), 0);
    shutdown(client, SD_SEND);
    closesocket(client);
}

}  // namespace

bool StartServerData(int httpPort, const std::string& enetHost, int enetPort) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(httpPort));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 16) != 0) {
        LogError("server_data: cannot listen on TCP %d (error %d). Is another server_data running?", httpPort,
                 WSAGetLastError());
        closesocket(listener);
        return false;
    }
    std::thread([listener, enetHost, enetPort] {
        for (;;) {
            SOCKET c = accept(listener, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            std::thread(Serve, c, enetHost, enetPort).detach();
        }
    }).detach();
    Log("server_data on TCP %d -> %s:%d", httpPort, enetHost.c_str(), enetPort);
    return true;
}
