#include "protocol/protocol_handler.hpp"

#include <algorithm>
#include <cctype>

namespace flashkv {

namespace {

using nlohmann::json;

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

// Every argument travels as a string internally, so numbers and booleans on the
// wire are stringified here rather than in each command handler.
std::string arg_to_string(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number_unsigned()) return std::to_string(v.get<uint64_t>());
    if (v.is_number_float()) return std::to_string(v.get<double>());
    return v.dump();  // objects/arrays: keep the raw text, handlers reject it
}

void append_scalar(std::vector<std::string>& args, const json& obj,
                   const char* field) {
    auto it = obj.find(field);
    if (it != obj.end() && !it->is_null()) args.push_back(arg_to_string(*it));
}

void append_array(std::vector<std::string>& args, const json& obj,
                  const char* field) {
    auto it = obj.find(field);
    if (it == obj.end() || !it->is_array()) return;
    for (const auto& element : *it) args.push_back(arg_to_string(element));
}

// {"f1": "v1"} becomes ["f1", "v1"]; handlers read the pairs back off.
void append_object_pairs(std::vector<std::string>& args, const json& obj,
                         const char* field) {
    auto it = obj.find(field);
    if (it == obj.end() || !it->is_object()) return;
    for (const auto& [k, v] : it->items()) {
        args.push_back(k);
        args.push_back(arg_to_string(v));
    }
}

}  // namespace

Result<Request> ProtocolHandler::parse_request(const std::string& raw) {
    json parsed;
    try {
        parsed = json::parse(raw);
    } catch (const json::exception& e) {
        return make_error<Request>(std::string("Malformed JSON: ") + e.what());
    }

    if (!parsed.is_object()) {
        return make_error<Request>("Request must be a JSON object");
    }

    auto cmd_it = parsed.find("cmd");
    if (cmd_it == parsed.end() || !cmd_it->is_string()) {
        return make_error<Request>("Missing required string field 'cmd'");
    }

    Request req;
    req.cmd = to_upper(cmd_it->get<std::string>());
    if (req.cmd.empty()) return make_error<Request>("Field 'cmd' must not be empty");

    // Two accepted argument shapes:
    //   1. an explicit flat list: {"cmd": "SET", "args": ["x", "y"]}
    //   2. named fields: {"cmd": "SET", "key": "x", "value": "y"}
    // Form 1 wins when both are present.
    auto args_it = parsed.find("args");
    if (args_it != parsed.end() && args_it->is_array()) {
        append_array(req.args, parsed, "args");
        return req;
    }

    // Fixed order, because handlers read args positionally.
    append_scalar(req.args, parsed, "key");
    append_array(req.args, parsed, "keys");
    append_scalar(req.args, parsed, "field");
    append_scalar(req.args, parsed, "member");
    append_scalar(req.args, parsed, "value");
    append_object_pairs(req.args, parsed, "fields");
    append_array(req.args, parsed, "elements");
    append_array(req.args, parsed, "members");
    append_scalar(req.args, parsed, "ttl");
    append_scalar(req.args, parsed, "seconds");
    append_scalar(req.args, parsed, "count");
    append_scalar(req.args, parsed, "start");
    append_scalar(req.args, parsed, "stop");

    return req;
}

std::string ProtocolHandler::build_response(const Response& resp) {
    return resp.to_json().dump() + "\n";
}

}  // namespace flashkv
