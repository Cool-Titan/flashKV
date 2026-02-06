#include "storage/lru_cache.hpp"

namespace flashkv {

void LRUCache::touch(const std::string& key) {
    std::lock_guard<std::mutex> lock(lru_mutex);

    auto it = key_to_iterator.find(key);
    if (it != key_to_iterator.end()) {
        // Already tracked: splice the existing node to the back. O(1), and the
        // iterator stays valid so the index needs no update.
        lru_list.splice(lru_list.end(), lru_list, it->second);
        return;
    }

    lru_list.push_back(key);
    key_to_iterator[key] = std::prev(lru_list.end());
}

std::optional<std::string> LRUCache::get_lru_key() const {
    std::lock_guard<std::mutex> lock(lru_mutex);
    if (lru_list.empty()) return std::nullopt;
    return lru_list.front();
}

std::optional<std::string> LRUCache::get_mru_key() const {
    std::lock_guard<std::mutex> lock(lru_mutex);
    if (lru_list.empty()) return std::nullopt;
    return lru_list.back();
}

void LRUCache::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(lru_mutex);
    auto it = key_to_iterator.find(key);
    if (it == key_to_iterator.end()) return;
    lru_list.erase(it->second);
    key_to_iterator.erase(it);
}

size_t LRUCache::size() const {
    std::lock_guard<std::mutex> lock(lru_mutex);
    return lru_list.size();
}

std::vector<std::string> LRUCache::get_order() const {
    std::lock_guard<std::mutex> lock(lru_mutex);
    return std::vector<std::string>(lru_list.begin(), lru_list.end());
}

}  // namespace flashkv
