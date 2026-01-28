#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>

namespace flashkv {

class CommandRouter;

// Handles one client socket for its whole lifetime. run() is meant to be the
// body of a dedicated thread; the connection owns the fd and closes it on
// destruction.
class Connection {
public:
    Connection(int socket_fd, CommandRouter* router);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Read/execute/respond until the peer closes, errors out, or shutdown()
    // is called from another thread.
    void run();
    void shutdown();

    int get_id() const { return connection_id; }

    // Pulls the first complete top-level JSON object out of buf, erasing it
    // from buf. Returns nullopt when the buffer holds only a partial object.
    // Exposed for testing; brace counting is the whole framing strategy.
    static std::optional<std::string> extract_json_object(std::string& buf);

private:
    int socket_fd;
    CommandRouter* router;
    int connection_id;
    std::atomic<bool> running;
    std::mutex conn_mutex;  // guards socket_fd close/shutdown

    static constexpr size_t BUFFER_SIZE = 4096;
    char read_buffer[BUFFER_SIZE];
    std::string pending_input;   // bytes read but not yet framed
    std::string write_buffer;

    static std::atomic<int> connection_counter;

    bool read_request();
    bool send_response(const std::string& response);
    void handle_error(const std::string& error_msg);
    void close_socket();

    void log_info(const std::string& msg);
    void log_error(const std::string& msg);
};

}  // namespace flashkv
