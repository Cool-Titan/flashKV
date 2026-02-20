#include <gtest/gtest.h>

#include "protocol/protocol_handler.hpp"

namespace flashkv::test {

class JsonProtocolTest : public ::testing::Test {};

TEST_F(JsonProtocolTest, ParseSimpleSetCommand) {
    std::string json = R"({"cmd": "SET", "key": "x", "value": "y"})";
    auto result = ProtocolHandler::parse_request(json);
    ASSERT_FALSE(is_error(result));
    auto req = get_value(result);
    EXPECT_EQ(req.cmd, "SET");
    ASSERT_EQ(req.args.size(), 2u);
    EXPECT_EQ(req.args[0], "x");
    EXPECT_EQ(req.args[1], "y");
}

TEST_F(JsonProtocolTest, ParseGetCommand) {
    std::string json = R"({"cmd": "GET", "key": "x"})";
    auto result = ProtocolHandler::parse_request(json);
    ASSERT_FALSE(is_error(result));
    auto req = get_value(result);
    EXPECT_EQ(req.cmd, "GET");
    ASSERT_EQ(req.args.size(), 1u);
    EXPECT_EQ(req.args[0], "x");
}

TEST_F(JsonProtocolTest, ParseHashSetCommand) {
    std::string json =
        R"({"cmd": "HSET", "key": "h", "fields": {"f1": "v1", "f2": "v2"}})";
    auto result = ProtocolHandler::parse_request(json);
    ASSERT_FALSE(is_error(result));
    auto req = get_value(result);
    // key followed by flattened field/value pairs, sorted by field name.
    ASSERT_EQ(req.args.size(), 5u);
    EXPECT_EQ(req.args[0], "h");
    EXPECT_EQ(req.args[1], "f1");
    EXPECT_EQ(req.args[2], "v1");
    EXPECT_EQ(req.args[3], "f2");
    EXPECT_EQ(req.args[4], "v2");
}

TEST_F(JsonProtocolTest, ParseListCommand) {
    std::string json = R"({"cmd": "LPUSH", "key": "list", "elements": ["a", "b"]})";
    auto result = ProtocolHandler::parse_request(json);
    ASSERT_FALSE(is_error(result));
    auto req = get_value(result);
    ASSERT_EQ(req.args.size(), 3u);
    EXPECT_EQ(req.args[0], "list");
    EXPECT_EQ(req.args[1], "a");
    EXPECT_EQ(req.args[2], "b");
}

TEST_F(JsonProtocolTest, ParseSetCommand) {
    std::string json = R"({"cmd": "SADD", "key": "set", "members": ["x", "y"]})";
    auto result = ProtocolHandler::parse_request(json);
    ASSERT_FALSE(is_error(result));
    auto req = get_value(result);
    ASSERT_EQ(req.args.size(), 3u);
    EXPECT_EQ(req.args[0], "set");
}

TEST_F(JsonProtocolTest, ParseExplicitArgsArray) {
    std::string json = R"({"cmd": "del", "args": ["a", "b", "c"]})";
    auto result = ProtocolHandler::parse_request(json);
    ASSERT_FALSE(is_error(result));
    auto req = get_value(result);
    EXPECT_EQ(req.cmd, "DEL");  // commands are case-insensitive
    ASSERT_EQ(req.args.size(), 3u);
    EXPECT_EQ(req.args[2], "c");
}

TEST_F(JsonProtocolTest, ParseNumericArgumentsAsStrings) {
    std::string json = R"({"cmd": "LRANGE", "key": "l", "start": 0, "stop": -1})";
    auto result = ProtocolHandler::parse_request(json);
    ASSERT_FALSE(is_error(result));
    auto req = get_value(result);
    ASSERT_EQ(req.args.size(), 3u);
    EXPECT_EQ(req.args[1], "0");
    EXPECT_EQ(req.args[2], "-1");
}

TEST_F(JsonProtocolTest, ParseSetWithTTL) {
    std::string json = R"({"cmd": "SET", "key": "k", "value": "v", "ttl": 30})";
    auto result = ProtocolHandler::parse_request(json);
    ASSERT_FALSE(is_error(result));
    auto req = get_value(result);
    ASSERT_EQ(req.args.size(), 3u);
    EXPECT_EQ(req.args[2], "30");
}

TEST_F(JsonProtocolTest, MalformedJsonError) {
    auto result = ProtocolHandler::parse_request("{invalid json");
    EXPECT_TRUE(is_error(result));
}

TEST_F(JsonProtocolTest, MissingCommandFieldError) {
    auto result = ProtocolHandler::parse_request(R"({"key": "x"})");
    EXPECT_TRUE(is_error(result));
}

TEST_F(JsonProtocolTest, NonObjectRequestError) {
    auto result = ProtocolHandler::parse_request(R"(["SET", "x", "y"])");
    EXPECT_TRUE(is_error(result));
}

TEST_F(JsonProtocolTest, EmptyCommandError) {
    auto result = ProtocolHandler::parse_request(R"({"cmd": ""})");
    EXPECT_TRUE(is_error(result));
}

TEST_F(JsonProtocolTest, SerializeStringResponse) {
    Response resp{"OK", std::nullopt, "hello"};
    std::string json = ProtocolHandler::build_response(resp);
    auto parsed = nlohmann::json::parse(json);
    EXPECT_EQ(parsed["status"], "OK");
    EXPECT_EQ(parsed["data"], "hello");
}

TEST_F(JsonProtocolTest, SerializeIntResponse) {
    Response resp{"OK", std::nullopt, 42};
    std::string json = ProtocolHandler::build_response(resp);
    auto parsed = nlohmann::json::parse(json);
    EXPECT_EQ(parsed["data"], 42);
}

TEST_F(JsonProtocolTest, SerializeErrorResponse) {
    Response resp{"ERROR", "Key not found", nullptr};
    std::string json = ProtocolHandler::build_response(resp);
    auto parsed = nlohmann::json::parse(json);
    EXPECT_EQ(parsed["status"], "ERROR");
    EXPECT_EQ(parsed["error_msg"], "Key not found");
    EXPECT_TRUE(parsed["data"].is_null());
}

TEST_F(JsonProtocolTest, ResponseIsNewlineTerminated) {
    Response resp{"OK", std::nullopt, nullptr};
    std::string json = ProtocolHandler::build_response(resp);
    ASSERT_FALSE(json.empty());
    EXPECT_EQ(json.back(), '\n');
}

}  // namespace flashkv::test
