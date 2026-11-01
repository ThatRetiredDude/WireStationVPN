// The app's line to the background part, over the console's own loopback:
// its status and buttons (HTTP on 127.0.0.1:9039), and starting it (the
// jailbreak's ELF loader on 127.0.0.1:9021).
#pragma once
#include <string>

namespace ws {

constexpr int kStatusPort = 9039, kLoaderPort = 9021;

// A request to 127.0.0.1:port. True with the body on a 200 answer; false
// with err when nothing answered or the answer wasn't 200.
bool http(const char *method, const std::string &path, std::string &body, std::string &err, int port = kStatusPort);

// Sends the payload file to the ELF loader, which starts it in a process of
// its own that stays after the app closes.
bool send_elf(const std::string &file, std::string &err, int port = kLoaderPort);

}  // namespace ws
