#include "command/command_router.hpp"

#include <optional>

#include "storage/lru_cache.hpp"
#include "utils/logger.hpp"

namespace flashkv {

namespace {

// std::optional has no nlohmann serializer, so it gets an explicit overload.
// Everything else goes through json's own conversions.
nlohmann::json value_json(const std::optional<std::string>& v) {
    return v ? nlohmann::json(*v) : nlohmann::json(nullptr);
}

template <typename T>
nlohmann::json value_json(const T& v) {
    return nlohmann::json(v);
}

template <typename T>
Response from_result(const Result<T>& r) {
    if (is_error(r)) return Response{"ERROR", get_error(r), nullptr};
    return Response{"OK", std::nullopt, value_json(get_value(r))};
}

std::optional<int> parse_int(const std::string& s) {
    try {
        size_t consumed = 0;
        const int v = std::stoi(s, &consumed);
        if (consumed != s.size()) return std::nullopt;
        return v;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<std::string> tail(const std::vector<std::string>& args, size_t from) {
    return std::vector<std::string>(args.begin() + static_cast<long>(from),
                                    args.end());
}

}  // namespace

CommandRouter::CommandRouter(KeyValueStore* store, LRUCache* lru)
    : store(store), lru(lru) {
    handlers = {
        {"PING", &CommandRouter::handle_ping},
        {"ECHO", &CommandRouter::handle_echo},
        {"SET", &CommandRouter::handle_set},
        {"GET", &CommandRouter::handle_get},
        {"DEL", &CommandRouter::handle_del},
        {"EXISTS", &CommandRouter::handle_exists},
        {"INCR", &CommandRouter::handle_incr},
        {"DECR", &CommandRouter::handle_decr},
        {"EXPIRE", &CommandRouter::handle_expire},
        {"TTL", &CommandRouter::handle_ttl},
        {"TYPE", &CommandRouter::handle_type},
        {"KEYS", &CommandRouter::handle_keys},
        {"DBSIZE", &CommandRouter::handle_dbsize},
        {"HSET", &CommandRouter::handle_hset},
        {"HGET", &CommandRouter::handle_hget},
        {"HDEL", &CommandRouter::handle_hdel},
        {"HEXISTS", &CommandRouter::handle_hexists},
        {"HGETALL", &CommandRouter::handle_hgetall},
        {"LPUSH", &CommandRouter::handle_lpush},
        {"RPUSH", &CommandRouter::handle_rpush},
        {"LPOP", &CommandRouter::handle_lpop},
        {"RPOP", &CommandRouter::handle_rpop},
        {"LLEN", &CommandRouter::handle_llen},
        {"LRANGE", &CommandRouter::handle_lrange},
        {"SADD", &CommandRouter::handle_sadd},
        {"SREM", &CommandRouter::handle_srem},
        {"SMEMBERS", &CommandRouter::handle_smembers},
        {"SISMEMBER", &CommandRouter::handle_sismember},
        {"SINTER", &CommandRouter::handle_sinter},
        {"SUNION", &CommandRouter::handle_sunion},
        {"LRUORDER", &CommandRouter::handle_lru_order},
    };
}

Response CommandRouter::ok(nlohmann::json data) {
    return Response{"OK", std::nullopt, std::move(data)};
}

Response CommandRouter::error(const std::string& msg) {
    return Response{"ERROR", msg, nullptr};
}

Response CommandRouter::wrong_arity(const std::string& cmd) {
    return error("ERR wrong number of arguments for '" + cmd + "' command");
}

Response CommandRouter::execute(const Request& req) {
    auto it = handlers.find(req.cmd);
    if (it == handlers.end()) {
        return error("ERR unknown command '" + req.cmd + "'");
    }
    Logger::debug("Executing command " + req.cmd + " with " +
                  std::to_string(req.args.size()) + " arg(s)");
    return (this->*(it->second))(req.args);
}

// ===== STRING / generic =====

Response CommandRouter::handle_ping(const std::vector<std::string>& args) {
    if (!args.empty()) return ok(args[0]);
    return Response{"PONG", std::nullopt, "PONG"};
}

Response CommandRouter::handle_echo(const std::vector<std::string>& args) {
    if (args.size() != 1) return wrong_arity("ECHO");
    return ok(args[0]);
}

Response CommandRouter::handle_set(const std::vector<std::string>& args) {
    // SET key value [ttl_seconds]
    if (args.size() < 2 || args.size() > 3) return wrong_arity("SET");

    std::optional<int> ttl;
    if (args.size() == 3) {
        ttl = parse_int(args[2]);
        if (!ttl || *ttl <= 0) return error("ERR invalid TTL, expected positive integer");
    }
    return from_result(store->set(args[0], args[1], ttl));
}

Response CommandRouter::handle_get(const std::vector<std::string>& args) {
    if (args.size() != 1) return wrong_arity("GET");
    return from_result(store->get(args[0]));
}

Response CommandRouter::handle_del(const std::vector<std::string>& args) {
    if (args.empty()) return wrong_arity("DEL");
    return from_result(store->del(args));
}

Response CommandRouter::handle_exists(const std::vector<std::string>& args) {
    if (args.empty()) return wrong_arity("EXISTS");
    return from_result(store->exists(args));
}

Response CommandRouter::handle_incr(const std::vector<std::string>& args) {
    if (args.size() != 1) return wrong_arity("INCR");
    return from_result(store->incr(args[0]));
}

Response CommandRouter::handle_decr(const std::vector<std::string>& args) {
    if (args.size() != 1) return wrong_arity("DECR");
    return from_result(store->decr(args[0]));
}

Response CommandRouter::handle_expire(const std::vector<std::string>& args) {
    if (args.size() != 2) return wrong_arity("EXPIRE");
    const auto seconds = parse_int(args[1]);
    if (!seconds) return error("ERR seconds must be an integer");
    return from_result(store->expire(args[0], *seconds));
}

Response CommandRouter::handle_ttl(const std::vector<std::string>& args) {
    if (args.size() != 1) return wrong_arity("TTL");
    return from_result(store->ttl(args[0]));
}

Response CommandRouter::handle_type(const std::vector<std::string>& args) {
    if (args.size() != 1) return wrong_arity("TYPE");
    return from_result(store->type(args[0]));
}

Response CommandRouter::handle_keys(const std::vector<std::string>& args) {
    if (!args.empty()) return wrong_arity("KEYS");
    return ok(store->keys());
}

Response CommandRouter::handle_dbsize(const std::vector<std::string>& args) {
    if (!args.empty()) return wrong_arity("DBSIZE");
    return ok(store->size());
}

// ===== HASH =====

Response CommandRouter::handle_hset(const std::vector<std::string>& args) {
    // HSET key field value [field value ...]
    if (args.size() < 3 || (args.size() - 1) % 2 != 0) return wrong_arity("HSET");

    std::map<std::string, std::string> fields;
    for (size_t i = 1; i + 1 < args.size(); i += 2) {
        fields[args[i]] = args[i + 1];
    }
    return from_result(store->hset(args[0], fields));
}

Response CommandRouter::handle_hget(const std::vector<std::string>& args) {
    if (args.size() != 2) return wrong_arity("HGET");
    return from_result(store->hget(args[0], args[1]));
}

Response CommandRouter::handle_hdel(const std::vector<std::string>& args) {
    if (args.size() < 2) return wrong_arity("HDEL");
    return from_result(store->hdel(args[0], tail(args, 1)));
}

Response CommandRouter::handle_hexists(const std::vector<std::string>& args) {
    if (args.size() != 2) return wrong_arity("HEXISTS");
    return from_result(store->hexists(args[0], args[1]));
}

Response CommandRouter::handle_hgetall(const std::vector<std::string>& args) {
    if (args.size() != 1) return wrong_arity("HGETALL");
    return from_result(store->hgetall(args[0]));
}

// ===== LIST =====

Response CommandRouter::handle_lpush(const std::vector<std::string>& args) {
    if (args.size() < 2) return wrong_arity("LPUSH");
    return from_result(store->lpush(args[0], tail(args, 1)));
}

Response CommandRouter::handle_rpush(const std::vector<std::string>& args) {
    if (args.size() < 2) return wrong_arity("RPUSH");
    return from_result(store->rpush(args[0], tail(args, 1)));
}

Response CommandRouter::handle_lpop(const std::vector<std::string>& args) {
    if (args.empty() || args.size() > 2) return wrong_arity("LPOP");

    int count = 1;
    if (args.size() == 2) {
        const auto parsed = parse_int(args[1]);
        if (!parsed || *parsed < 0) return error("ERR count must be a non-negative integer");
        count = *parsed;
    }
    return from_result(store->lpop(args[0], count));
}

Response CommandRouter::handle_rpop(const std::vector<std::string>& args) {
    if (args.empty() || args.size() > 2) return wrong_arity("RPOP");

    int count = 1;
    if (args.size() == 2) {
        const auto parsed = parse_int(args[1]);
        if (!parsed || *parsed < 0) return error("ERR count must be a non-negative integer");
        count = *parsed;
    }
    return from_result(store->rpop(args[0], count));
}

Response CommandRouter::handle_llen(const std::vector<std::string>& args) {
    if (args.size() != 1) return wrong_arity("LLEN");
    return from_result(store->llen(args[0]));
}

Response CommandRouter::handle_lrange(const std::vector<std::string>& args) {
    if (args.size() != 3) return wrong_arity("LRANGE");

    const auto start = parse_int(args[1]);
    const auto stop = parse_int(args[2]);
    if (!start || !stop) return error("ERR start and stop must be integers");
    return from_result(store->lrange(args[0], *start, *stop));
}

// ===== SET =====

Response CommandRouter::handle_sadd(const std::vector<std::string>& args) {
    if (args.size() < 2) return wrong_arity("SADD");
    return from_result(store->sadd(args[0], tail(args, 1)));
}

Response CommandRouter::handle_srem(const std::vector<std::string>& args) {
    if (args.size() < 2) return wrong_arity("SREM");
    return from_result(store->srem(args[0], tail(args, 1)));
}

Response CommandRouter::handle_smembers(const std::vector<std::string>& args) {
    if (args.size() != 1) return wrong_arity("SMEMBERS");
    return from_result(store->smembers(args[0]));
}

Response CommandRouter::handle_sismember(const std::vector<std::string>& args) {
    if (args.size() != 2) return wrong_arity("SISMEMBER");
    return from_result(store->sismember(args[0], args[1]));
}

Response CommandRouter::handle_sinter(const std::vector<std::string>& args) {
    if (args.empty()) return wrong_arity("SINTER");
    return from_result(store->sinter(args));
}

Response CommandRouter::handle_sunion(const std::vector<std::string>& args) {
    if (args.empty()) return wrong_arity("SUNION");
    return from_result(store->sunion(args));
}

// ===== LRU introspection =====

Response CommandRouter::handle_lru_order(const std::vector<std::string>& args) {
    if (!args.empty()) return wrong_arity("LRUORDER");
    if (!lru) return error("ERR LRU tracking is not enabled");
    return ok(lru->get_order());
}

}  // namespace flashkv
