#pragma once
#include <string>

// Starts the server_data.php listener on its own thread. Needs WSAStartup done
// (enet_initialize does it).
bool StartServerData(int httpPort, const std::string& enetHost, int enetPort);
