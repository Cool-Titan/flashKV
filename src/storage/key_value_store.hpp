#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "storage/types.hpp"

namespace flashkv {

class LRUCache;

// Core in-memory store. Every public method takes the single store mutex, so
// the whole store is one critical section.
// ponytail: one global mutex; shard by key hash if lock contention ever shows up.
//
// Expiration is lazy: a key is only removed when it is looked at after its
// expiry time has passed.
class KeyValueStore {
public:
    KeyValueStore() = default;
    ~KeyValueStore() = default;

    KeyValueStore(const KeyValueStore&) = delete;
    KeyValueStore& operator=(const KeyValueStore&) = delete;

    // ===== STRING =====
    Result<std::string> set(const std::string& key, const std::string& value,
                            std::optional<int> ttl_seconds = std::nullopt);
    Result<std::optional<std::string>> get(const std::string& key);
    Result<int> del(const std::vector<std::string>& keys);
    Result<int> exists(const std::vector<std::string>& keys);
    Result<int64_t> incr(const std::string& key);
    Result<int64_t> decr(const std::string& key);

    // ===== HASH =====
    Result<int> hset(const std::string& key,
                     const std::map<std::string, std::string>& fields);
    Result<std::optional<std::string>> hget(const std::string& key,
                                            const std::string& field);
    Result<int> hdel(const std::string& key,
                     const std::vector<std::string>& fields);
    Result<int> hexists(const std::string& key, const std::string& field);
    Result<std::map<std::string, std::string>> hgetall(const std::string& key);

    // ===== LIST =====
    Result<int> lpush(const std::string& key,
                      const std::vector<std::string>& elements);
    Result<int> rpush(const std::string& key,
                      const std::vector<std::string>& elements);
    Result<std::vector<std::string>> lpop(const std::string& key, int count = 1);
    Result<std::vector<std::string>> rpop(const std::string& key, int count = 1);
    Result<int> llen(const std::string& key);
    Result<std::vector<std::string>> lrange(const std::string& key, int start,
                                            int stop);

    // ===== SET =====
    Result<int> sadd(const std::string& key,
                     const std::vector<std::string>& members);
    Result<int> srem(const std::string& key,
                     const std::vector<std::string>& members);
    Result<std::vector<std::string>> smembers(const std::string& key);
    Result<int> sismember(const std::string& key, const std::string& member);
    Result<std::vector<std::string>> sinter(const std::vector<std::string>& keys);
    Result<std::vector<std::string>> sunion(const std::vector<std::string>& keys);

    // ===== TTL =====
    Result<int> expire(const std::string& key, int seconds);
    Result<int> ttl(const std::string& key);

    // ===== Introspection (used by the server's INFO/KEYS style commands) =====
    size_t size() const;
    std::vector<std::string> keys() const;
    Result<std::string> type(const std::string& key);

    // Non-owning reference; the LRU cache outlives the store in Server.
    void set_lru_cache(LRUCache* lru) { lru_cache = lru; }

private:
    using StoreMap = std::map<std::string, KeyMetadata>;

    StoreMap store;
    mutable std::mutex store_mutex;
    LRUCache* lru_cache = nullptr;

    // All helpers below assume store_mutex is already held.

    // Returns an iterator to a live (non-expired) key, dropping it first if it
    // has expired. Returns store.end() when the key is absent or expired.
    StoreMap::iterator find_live_unlocked(const std::string& key);

    bool key_exists_unlocked(const std::string& key);
    void cleanup_expired_key_unlocked(const std::string& key);
    void update_access_time_unlocked(const std::string& key);

    void erase_unlocked(StoreMap::iterator it);
    void touch_unlocked(const std::string& key);
};

}  // namespace flashkv
