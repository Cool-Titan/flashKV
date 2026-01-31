#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "command/command_router.hpp"
#include "storage/key_value_store.hpp"
#include "storage/lru_cache.hpp"

namespace flashkv {

class Connection;

// TCP front end: owns the store, accepts connections, and hands each one to a
// dedicated thread.
// ponytail: thread-per-connection; swap in epoll if the client count ever
// outgrows what the OS will schedule comfortably.
class Server {
public:
    explicit Server(int port = 6379);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds, listens, and blocks until shutdown (SIGINT/SIGTERM or shutdown()).
    void start();
    void shutdown();
    bool is_running() const;

    int get_port() const { return port; }

private:
    int port;
    int listen_socket_fd;
    std::atomic<bool> running;
    std::vector<std::thread> worker_threads;  // holds the accept thread
    std::mutex server_mutex;

    std::unique_ptr<KeyValueStore> store;
    std::unique_ptr<LRUCache> lru;
    std::unique_ptr<CommandRouter> router;

    // Live client connections. Handler threads are detached, so this registry
    // is what lets shutdown() unblock them and wait for them to finish;
    // without it a thread could still be using the router after the Server is
    // destroyed. It is shared (not a plain member) so a handler thread can
    // safely deregister itself even if it is the very last one out.
    struct ConnectionRegistry {
        std::mutex mutex;
        std::vector<std::shared_ptr<Connection>> connections;
    };
    std::shared_ptr<ConnectionRegistry> registry;

    void accept_loop();
    void close_all_connections();
    void wait_for_connections_to_drain();
    static void setup_signal_handlers();
    static std::atomic<bool> should_shutdown;
    static void signal_handler(int sig);

    void log_info(const std::string& msg);
    void log_error(const std::string& msg);
};

}  // namespace flashkv
