#pragma once

#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace flashkv {

class KeyValueStore;

// Tracks key access order. LRU at the front of the list, MRU at the back.
// Ordering only: nothing is evicted, memory limits are out of scope.
class LRUCache {
public:
    explicit LRUCache(KeyValueStore* store) : store_ptr(store) {}

    // Mark key as most recently used (inserts it if unknown).
    void touch(const std::string& key);

    std::optional<std::string> get_lru_key() const;
    std::optional<std::string> get_mru_key() const;

    // Stop tracking a key (called when the key is deleted or expires).
    void remove(const std::string& key);

    size_t size() const;

    // Full order, oldest first / newest last.
    std::vector<std::string> get_order() const;

private:
    std::list<std::string> lru_list;  // MRU at end, LRU at front
    std::map<std::string, std::list<std::string>::iterator> key_to_iterator;
    mutable std::mutex lru_mutex;

    // Non-owning back-pointer to the store this cache tracks. Unused today;
    // kept because eviction (future work) needs to delete from the store.
    KeyValueStore* store_ptr;
};

}  // namespace flashkv
