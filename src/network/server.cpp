#include "network/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "network/connection.hpp"
#include "utils/logger.hpp"

namespace flashkv {

namespace {
constexpr int LISTEN_BACKLOG = 128;
// Short poll timeout so the accept loop notices shutdown promptly instead of
// sitting in a blocking accept().
constexpr int ACCEPT_POLL_TIMEOUT_MS = 200;
}  // namespace

std::atomic<bool> Server::should_shutdown{false};

Server::Server(int port)
    : port(port),
      listen_socket_fd(-1),
      running(false),
      store(std::make_unique<KeyValueStore>()),
      registry(std::make_shared<ConnectionRegistry>()) {
    lru = std::make_unique<LRUCache>(store.get());
    store->set_lru_cache(lru.get());
    router = std::make_unique<CommandRouter>(store.get(), lru.get());
}

Server::~Server() {
    shutdown();
    for (auto& t : worker_threads) {
        if (t.joinable()) t.join();
    }
}

void Server::signal_handler(int sig) {
    // Async-signal-safe: only touch the atomic flag here.
    (void)sig;
    should_shutdown = true;
}

void Server::setup_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = &Server::signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // A client that disappears mid-write must not kill the process.
    signal(SIGPIPE, SIG_IGN);
}

bool Server::is_running() const {
    return running;
}

void Server::start() {
    should_shutdown = false;

    listen_socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_fd < 0) {
        throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
    }

    // Lets the server restart immediately after a crash instead of waiting out
    // TIME_WAIT on the listening port.
    int enable = 1;
    setsockopt(listen_socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(listen_socket_fd, reinterpret_cast<struct sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        const std::string err = std::string("bind() failed on port ") +
                                std::to_string(port) + ": " + std::strerror(errno);
        ::close(listen_socket_fd);
        listen_socket_fd = -1;
        throw std::runtime_error(err);
    }

    if (::listen(listen_socket_fd, LISTEN_BACKLOG) < 0) {
        const std::string err = std::string("listen() failed: ") + std::strerror(errno);
        ::close(listen_socket_fd);
        listen_socket_fd = -1;
        throw std::runtime_error(err);
    }

    setup_signal_handlers();
    running = true;
    log_info("FlashKV listening on port " + std::to_string(port));

    worker_threads.emplace_back(&Server::accept_loop, this);

    // Park here until a signal or an explicit shutdown() arrives.
    while (running && !should_shutdown) {
        struct timespec ts{0, 100 * 1000 * 1000};  // 100ms
        nanosleep(&ts, nullptr);
    }

    log_info("Shutdown requested");
    shutdown();

    for (auto& t : worker_threads) {
        if (t.joinable()) t.join();
    }
    worker_threads.clear();
    log_info("Server stopped");
}

void Server::accept_loop() {
    while (running && !should_shutdown) {
        struct pollfd pfd{};
        {
            std::lock_guard<std::mutex> lock(server_mutex);
            if (listen_socket_fd < 0) break;
            pfd.fd = listen_socket_fd;
        }
        pfd.events = POLLIN;

        const int ready = ::poll(&pfd, 1, ACCEPT_POLL_TIMEOUT_MS);
        if (ready == 0) continue;  // timed out, re-check the shutdown flags
        if (ready < 0) {
            if (errno == EINTR) continue;
            log_error(std::string("poll() failed: ") + std::strerror(errno));
            break;
        }

        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        const int client_fd = ::accept(
            pfd.fd, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);

        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (!running || should_shutdown) break;  // socket closed under us
            log_error(std::string("accept() failed: ") + std::strerror(errno));
            continue;
        }

        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        log_info(std::string("Accepted client from ") + ip + ":" +
                 std::to_string(ntohs(client_addr.sin_port)));

        // Small requests should not wait on Nagle's algorithm.
        int nodelay = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // Detached thread per client. The connection is registered first so a
        // shutdown can reach in and unblock it.
        auto conn = std::make_shared<Connection>(client_fd, router.get());
        {
            std::lock_guard<std::mutex> lock(registry->mutex);
            registry->connections.push_back(conn);
        }

        // The thread keeps the registry alive on its own, so deregistering is
        // safe even after the Server has gone away.
        std::thread([conn, reg = registry]() {
            conn->run();
            std::lock_guard<std::mutex> lock(reg->mutex);
            auto& live = reg->connections;
            for (auto it = live.begin(); it != live.end(); ++it) {
                if (*it == conn) {
                    live.erase(it);
                    break;
                }
            }
        }).detach();
    }

    running = false;
}

void Server::shutdown() {
    // Safe to call twice: the socket close below is guarded by the fd check.
    running = false;
    should_shutdown = true;

    {
        std::lock_guard<std::mutex> lock(server_mutex);
        if (listen_socket_fd >= 0) {
            ::shutdown(listen_socket_fd, SHUT_RDWR);
            ::close(listen_socket_fd);
            listen_socket_fd = -1;
        }
    }

    close_all_connections();
    wait_for_connections_to_drain();
}

void Server::close_all_connections() {
    // Copy under the lock, then act outside it: Connection::shutdown() wakes a
    // handler thread that wants the same lock to deregister itself.
    std::vector<std::shared_ptr<Connection>> live;
    {
        std::lock_guard<std::mutex> lock(registry->mutex);
        live = registry->connections;
    }
    for (auto& conn : live) conn->shutdown();
}

void Server::wait_for_connections_to_drain() {
    // Handler threads are detached, so this is the only join-like guarantee
    // that none of them still points at the router we are about to destroy.
    for (int waited_ms = 0; waited_ms < 5000; waited_ms += 10) {
        {
            std::lock_guard<std::mutex> lock(registry->mutex);
            if (registry->connections.empty()) return;
        }
        struct timespec ts{0, 10 * 1000 * 1000};  // 10ms
        nanosleep(&ts, nullptr);
    }
    log_error("Timed out waiting for client connections to close");
}

void Server::log_info(const std::string& msg) {
    Logger::info("[server] " + msg);
}

void Server::log_error(const std::string& msg) {
    Logger::error("[server] " + msg);
}

}  // namespace flashkv
