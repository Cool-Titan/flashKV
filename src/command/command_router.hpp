#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "protocol/protocol_handler.hpp"
#include "storage/key_value_store.hpp"

namespace flashkv {

class LRUCache;

// Dispatches a parsed Request to the matching store operation and wraps the
// outcome in a Response. Stateless apart from the two non-owning pointers, so
// every connection thread can share one router.
class CommandRouter {
public:
    CommandRouter(KeyValueStore* store, LRUCache* lru);

    Response execute(const Request& req);

private:
    using Handler = Response (CommandRouter::*)(const std::vector<std::string>&);

    KeyValueStore* store;
    LRUCache* lru;
    std::unordered_map<std::string, Handler> handlers;

    // ===== STRING / generic =====
    Response handle_ping(const std::vector<std::string>& args);
    Response handle_echo(const std::vector<std::string>& args);
    Response handle_set(const std::vector<std::string>& args);
    Response handle_get(const std::vector<std::string>& args);
    Response handle_del(const std::vector<std::string>& args);
    Response handle_exists(const std::vector<std::string>& args);
    Response handle_incr(const std::vector<std::string>& args);
    Response handle_decr(const std::vector<std::string>& args);
    Response handle_expire(const std::vector<std::string>& args);
    Response handle_ttl(const std::vector<std::string>& args);
    Response handle_type(const std::vector<std::string>& args);
    Response handle_keys(const std::vector<std::string>& args);
    Response handle_dbsize(const std::vector<std::string>& args);

    // ===== HASH =====
    Response handle_hset(const std::vector<std::string>& args);
    Response handle_hget(const std::vector<std::string>& args);
    Response handle_hdel(const std::vector<std::string>& args);
    Response handle_hexists(const std::vector<std::string>& args);
    Response handle_hgetall(const std::vector<std::string>& args);

    // ===== LIST =====
    Response handle_lpush(const std::vector<std::string>& args);
    Response handle_rpush(const std::vector<std::string>& args);
    Response handle_lpop(const std::vector<std::string>& args);
    Response handle_rpop(const std::vector<std::string>& args);
    Response handle_llen(const std::vector<std::string>& args);
    Response handle_lrange(const std::vector<std::string>& args);

    // ===== SET =====
    Response handle_sadd(const std::vector<std::string>& args);
    Response handle_srem(const std::vector<std::string>& args);
    Response handle_smembers(const std::vector<std::string>& args);
    Response handle_sismember(const std::vector<std::string>& args);
    Response handle_sinter(const std::vector<std::string>& args);
    Response handle_sunion(const std::vector<std::string>& args);

    // ===== LRU introspection =====
    Response handle_lru_order(const std::vector<std::string>& args);

    static Response ok(nlohmann::json data);
    static Response error(const std::string& msg);
    static Response wrong_arity(const std::string& cmd);
};

}  // namespace flashkv
