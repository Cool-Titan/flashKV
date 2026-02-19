#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "command/command_router.hpp"
#include "network/server.hpp"
#include "protocol/protocol_handler.hpp"
#include "storage/key_value_store.hpp"
#include "storage/lru_cache.hpp"

namespace flashkv::test {

class IntegrationTest : public ::testing::Test {
protected:
    KeyValueStore store;
};

TEST_F(IntegrationTest, EndToEndStringOps) {
    store.set("foo", "bar");
    auto get_result = store.get("foo");
    EXPECT_EQ(get_value(get_result), "bar");

    auto del_result = store.del({"foo"});
    EXPECT_EQ(get_value(del_result), 1);

    auto get_after_del = store.get("foo");
    EXPECT_FALSE(get_value(get_after_del).has_value());
}

TEST_F(IntegrationTest, MixedDataTypes) {
    store.set("str", "value");
    std::map<std::string, std::string> fields = {{"f", "v"}};
    store.hset("hash", fields);
    store.lpush("list", {"elem"});
    store.sadd("set", {"member"});

    auto str_result = store.get("str");
    auto hash_result = store.hgetall("hash");
    auto list_result = store.llen("list");
    auto set_result = store.smembers("set");

    EXPECT_FALSE(is_error(str_result));
    EXPECT_FALSE(is_error(hash_result));
    EXPECT_FALSE(is_error(list_result));
    EXPECT_FALSE(is_error(set_result));
}

TEST_F(IntegrationTest, TTLExpiration) {
    store.set("key", "value", 1);
    auto result1 = store.get("key");
    EXPECT_TRUE(get_value(result1).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto result2 = store.get("key");
    EXPECT_FALSE(get_value(result2).has_value());
}

TEST_F(IntegrationTest, LRUOrdering) {
    KeyValueStore test_store;
    LRUCache lru(&test_store);
    test_store.set_lru_cache(&lru);

    test_store.set("a", "1");
    test_store.set("b", "2");
    test_store.set("c", "3");

    auto order = lru.get_order();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "a");
    EXPECT_EQ(order[1], "b");
    EXPECT_EQ(order[2], "c");
}

// ===== Protocol -> router round trip (no socket involved) =====

TEST_F(IntegrationTest, ParseThenRouteSetAndGet) {
    LRUCache lru(&store);
    store.set_lru_cache(&lru);
    CommandRouter router(&store, &lru);

    auto set_req = ProtocolHandler::parse_request(
        R"({"cmd": "SET", "key": "k", "value": "v"})");
    ASSERT_FALSE(is_error(set_req));
    auto set_resp = router.execute(get_value(set_req));
    EXPECT_EQ(set_resp.status, "OK");

    auto get_req = ProtocolHandler::parse_request(R"({"cmd": "GET", "key": "k"})");
    ASSERT_FALSE(is_error(get_req));
    auto get_resp = router.execute(get_value(get_req));
    EXPECT_EQ(get_resp.status, "OK");
    EXPECT_EQ(get_resp.data, "v");
}

TEST_F(IntegrationTest, RouterRejectsUnknownCommand) {
    LRUCache lru(&store);
    CommandRouter router(&store, &lru);

    auto req = ProtocolHandler::parse_request(R"({"cmd": "NOPE"})");
    ASSERT_FALSE(is_error(req));
    auto resp = router.execute(get_value(req));
    EXPECT_EQ(resp.status, "ERROR");
}

TEST_F(IntegrationTest, RouterReportsWrongArity) {
    LRUCache lru(&store);
    CommandRouter router(&store, &lru);

    Request req{"SET", {"only_key"}};
    auto resp = router.execute(req);
    EXPECT_EQ(resp.status, "ERROR");
}

// ===== Real TCP round trip against a live server =====

namespace {

constexpr int TEST_PORT = 16379;

// Connects with a short retry window, since the server binds asynchronously.
int connect_to_server(int port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
}

// One client connection: keeps its own read buffer so a reply that arrives
// glued to the next one (pipelining, TCP coalescing) is still framed correctly.
class TestClient {
public:
    explicit TestClient(int fd) : fd(fd) {}
    ~TestClient() {
        if (fd >= 0) ::close(fd);
    }

    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    bool valid() const { return fd >= 0; }

    // Sends one request and returns the next reply line, parsed.
    nlohmann::json round_trip(const std::string& request) {
        if (::send(fd, request.data(), request.size(), 0) < 0) return nullptr;
        return next_reply();
    }

    nlohmann::json next_reply() {
        char chunk[4096];
        while (buffer.find('\n') == std::string::npos) {
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) return nullptr;
            buffer.append(chunk, static_cast<size_t>(n));
        }
        const size_t newline = buffer.find('\n');
        const std::string line = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        return nlohmann::json::parse(line, nullptr, false);
    }

private:
    int fd;
    std::string buffer;
};

// Runs a server for the lifetime of the scope. The destructor always stops and
// joins it, so an assertion that returns early cannot leave a joinable thread
// behind (which would abort the whole test binary).
class ScopedServer {
public:
    explicit ScopedServer(int port)
        : server(port), thread([this]() {
              try {
                  server.start();
              } catch (const std::exception& e) {
                  ADD_FAILURE() << "server failed to start: " << e.what();
              }
          }) {}

    ~ScopedServer() {
        server.shutdown();
        if (thread.joinable()) thread.join();
    }

    ScopedServer(const ScopedServer&) = delete;
    ScopedServer& operator=(const ScopedServer&) = delete;

private:
    Server server;       // declared first: it must outlive the thread using it
    std::thread thread;
};

}  // namespace

TEST(ServerIntegrationTest, ClientCanRunCommandsOverTcp) {
    ScopedServer server(TEST_PORT);

    TestClient client(connect_to_server(TEST_PORT));
    ASSERT_TRUE(client.valid()) << "could not connect to test server";

    auto set_reply = client.round_trip(R"({"cmd":"SET","key":"tcp","value":"works"})");
    ASSERT_TRUE(set_reply.is_object()) << "no reply to SET";
    EXPECT_EQ(set_reply["status"], "OK");

    auto get_reply = client.round_trip(R"({"cmd":"GET","key":"tcp"})");
    ASSERT_TRUE(get_reply.is_object());
    EXPECT_EQ(get_reply["data"], "works");

    auto err_reply = client.round_trip(R"({"cmd":"BOGUS"})");
    ASSERT_TRUE(err_reply.is_object());
    EXPECT_EQ(err_reply["status"], "ERROR");

    auto ping_reply = client.round_trip(R"({"cmd":"ping"})");
    ASSERT_TRUE(ping_reply.is_object());
    EXPECT_EQ(ping_reply["status"], "PONG");
}

TEST(ServerIntegrationTest, AnswersPipelinedRequestsInOrder) {
    ScopedServer server(TEST_PORT + 1);

    TestClient client(connect_to_server(TEST_PORT + 1));
    ASSERT_TRUE(client.valid());

    // Two requests in a single write must both get answered, in order.
    auto first = client.round_trip(
        R"({"cmd":"SET","key":"p","value":"1"}{"cmd":"GET","key":"p"})");
    ASSERT_TRUE(first.is_object());
    EXPECT_EQ(first["status"], "OK");

    auto second = client.next_reply();
    ASSERT_TRUE(second.is_object());
    EXPECT_EQ(second["data"], "1");
}

TEST(ServerIntegrationTest, RejectsMalformedJsonWithoutDroppingTheConnection) {
    ScopedServer server(TEST_PORT + 2);

    TestClient client(connect_to_server(TEST_PORT + 2));
    ASSERT_TRUE(client.valid());

    auto bad = client.round_trip("not json at all\n");
    ASSERT_TRUE(bad.is_object());
    EXPECT_EQ(bad["status"], "ERROR");

    // The connection stays usable after a bad request.
    auto good = client.round_trip(R"({"cmd":"SET","key":"after","value":"ok"})");
    ASSERT_TRUE(good.is_object());
    EXPECT_EQ(good["status"], "OK");
}

TEST(ServerIntegrationTest, HandlesManyConcurrentClients) {
    constexpr int port = TEST_PORT + 3;
    constexpr int client_count = 50;
    ScopedServer server(port);

    // Make sure it is up before fanning out.
    {
        TestClient probe(connect_to_server(port));
        ASSERT_TRUE(probe.valid());
    }

    std::atomic<int> ok_count{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < client_count; ++i) {
        clients.emplace_back([i, &ok_count]() {
            TestClient client(connect_to_server(port));
            if (!client.valid()) return;

            const std::string key = "c_" + std::to_string(i);
            auto reply = client.round_trip(
                R"({"cmd":"SET","key":")" + key + R"(","value":"v"})");
            if (!reply.is_object() || reply["status"] != "OK") return;

            auto echoed = client.round_trip(R"({"cmd":"GET","key":")" + key + R"("})");
            if (echoed.is_object() && echoed["data"] == "v") ++ok_count;
        });
    }
    for (auto& t : clients) t.join();

    EXPECT_EQ(ok_count.load(), client_count);
}

}  // namespace flashkv::test
