#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "storage/types.hpp"

namespace flashkv {

// A parsed client request: the command name (upper-cased) plus a flat argument
// list. The wire format is friendlier than that (named "key"/"value"/"fields"
// members), and parse_request() is what flattens it.
struct Request {
    std::string cmd;
    std::vector<std::string> args;
};

// A response ready to be serialized. Kept an aggregate so it can be built with
// Response{"OK", std::nullopt, data}.
struct Response {
    std::string status;  // "OK", "ERROR", "PONG"
    std::optional<std::string> error_msg;
    nlohmann::json data;

    nlohmann::json to_json() const {
        nlohmann::json j;
        j["status"] = status;
        if (error_msg) j["error_msg"] = *error_msg;
        j["data"] = data;
        return j;
    }
};

class ProtocolHandler {
public:
    // Parses one JSON object into a Request. Malformed JSON, a non-object
    // payload, or a missing/blank "cmd" all come back as errors.
    static Result<Request> parse_request(const std::string& raw);

    // Serializes a response. A trailing newline is appended so line-oriented
    // clients (telnet, nc) can frame replies.
    static std::string build_response(const Response& resp);
};

}  // namespace flashkv
