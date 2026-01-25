#include <cstdlib>
#include <iostream>
#include <string>

#include "network/server.hpp"
#include "utils/logger.hpp"

using namespace flashkv;

namespace {

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [--port PORT] [--debug]\n"
              << "  --port PORT   TCP port to listen on (default 6379)\n"
              << "  --debug       enable DEBUG level logging\n"
              << "  --help        show this message\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    int port = 6379;

    // FLASHKV_DEBUG=1 is how docker-compose turns debug logging on.
    if (const char* env_debug = std::getenv("FLASHKV_DEBUG")) {
        if (std::string(env_debug) == "1") Logger::set_debug_mode(true);
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            try {
                port = std::stoi(argv[++i]);
            } catch (const std::exception&) {
                std::cerr << "Invalid port: " << argv[i] << "\n";
                return 1;
            }
            if (port <= 0 || port > 65535) {
                std::cerr << "Port out of range: " << port << "\n";
                return 1;
            }
        } else if (arg == "--debug") {
            Logger::set_debug_mode(true);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    Logger::info("Starting FlashKV server on port " + std::to_string(port));

    try {
        Server server(port);
        server.start();
    } catch (const std::exception& e) {
        Logger::error(std::string("Server error: ") + e.what());
        return 1;
    }

    return 0;
}
