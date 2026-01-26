#include "network/connection.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "command/command_router.hpp"
#include "protocol/protocol_handler.hpp"
#include "utils/logger.hpp"

namespace flashkv {

namespace {
// A client that goes quiet for this long is dropped, so a dead peer cannot pin
// a thread forever.
constexpr int RECV_TIMEOUT_SECONDS = 30;
}  // namespace

std::atomic<int> Connection::connection_counter{0};

Connection::Connection(int socket_fd, CommandRouter* router)
    : socket_fd(socket_fd),
      router(router),
      connection_id(++connection_counter),
      running(true),
      read_buffer{} {
    // Without a timeout an idle client would hold its thread indefinitely.
    struct timeval tv{};
    tv.tv_sec = RECV_TIMEOUT_SECONDS;
    tv.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    log_info("Connection opened");
}

Connection::~Connection() {
    close_socket();
    log_info("Connection closed");
}

void Connection::close_socket() {
    std::lock_guard<std::mutex> lock(conn_mutex);
    if (socket_fd >= 0) {
        ::close(socket_fd);
        socket_fd = -1;
    }
}

void Connection::shutdown() {
    running = false;
    // Unblocks a thread parked in recv().
    std::lock_guard<std::mutex> lock(conn_mutex);
    if (socket_fd >= 0) ::shutdown(socket_fd, SHUT_RDWR);
}

std::optional<std::string> Connection::extract_json_object(std::string& buf) {
    // Requests are whitespace-separated JSON objects with no length prefix, so
    // framing means counting braces while ignoring anything inside a string.
    size_t start = buf.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        buf.clear();
        return std::nullopt;
    }
    if (buf[start] != '{') {
        // Junk that can never become a valid object: hand it to the parser so
        // the client gets a proper error instead of a silent stall.
        std::string junk = buf.substr(start);
        buf.clear();
        return junk;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (size_t i = start; i < buf.size(); ++i) {
        const char c = buf[i];

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) {
                std::string obj = buf.substr(start, i - start + 1);
                buf.erase(0, i + 1);
                return obj;
            }
        }
    }

    return std::nullopt;  // incomplete, wait for more bytes
}

bool Connection::read_request() {
    const ssize_t bytes = ::recv(socket_fd, read_buffer, BUFFER_SIZE, 0);

    if (bytes > 0) {
        pending_input.append(read_buffer, static_cast<size_t>(bytes));
        return true;
    }
    if (bytes == 0) {
        log_info("Peer closed the connection");
        return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        log_info("Idle timeout, closing connection");
        return false;
    }
    if (errno == EINTR) return true;  // retry

    handle_error(std::string("recv failed: ") + std::strerror(errno));
    return false;
}

bool Connection::send_response(const std::string& response) {
    size_t sent = 0;
    while (sent < response.size()) {
        const ssize_t n = ::send(socket_fd, response.data() + sent,
                                 response.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        handle_error(std::string("send failed: ") + std::strerror(errno));
        return false;
    }
    return true;
}

void Connection::handle_error(const std::string& error_msg) {
    log_error(error_msg);
}

void Connection::run() {
    while (running) {
        if (!read_request()) break;

        // One read can carry several pipelined requests, or half of one.
        while (running) {
            auto frame = extract_json_object(pending_input);
            if (!frame) break;

            Response response;
            auto parsed = ProtocolHandler::parse_request(*frame);
            if (is_error(parsed)) {
                response = Response{"ERROR", get_error(parsed), nullptr};
                log_error("Bad request: " + get_error(parsed));
            } else {
                response = router->execute(get_value(parsed));
            }

            write_buffer = ProtocolHandler::build_response(response);
            if (!send_response(write_buffer)) {
                running = false;
                break;
            }
        }
    }

    running = false;
    close_socket();
}

void Connection::log_info(const std::string& msg) {
    Logger::info("[conn " + std::to_string(connection_id) + "] " + msg);
}

void Connection::log_error(const std::string& msg) {
    Logger::error("[conn " + std::to_string(connection_id) + "] " + msg);
}

}  // namespace flashkv
