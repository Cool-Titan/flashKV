#include "storage/key_value_store.hpp"

#include <algorithm>
#include <cmath>

#include "storage/lru_cache.hpp"

namespace flashkv {

namespace {

const char* const NOT_INTEGER_ERROR = "value is not an integer or out of range";

// Parses a whole string as a 64-bit integer. Returns nullopt on any junk,
// including trailing characters ("12abc" is not 12).
std::optional<int64_t> parse_int64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try {
        size_t consumed = 0;
        const int64_t v = std::stoll(s, &consumed);
        if (consumed != s.size()) return std::nullopt;
        return v;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

// ===== Private helpers (all assume store_mutex is held) =====

KeyValueStore::StoreMap::iterator KeyValueStore::find_live_unlocked(
    const std::string& key) {
    auto it = store.find(key);
    if (it == store.end()) return store.end();
    if (it->second.is_expired()) {
        erase_unlocked(it);
        return store.end();
    }
    return it;
}

bool KeyValueStore::key_exists_unlocked(const std::string& key) {
    return find_live_unlocked(key) != store.end();
}

void KeyValueStore::cleanup_expired_key_unlocked(const std::string& key) {
    auto it = store.find(key);
    if (it != store.end() && it->second.is_expired()) erase_unlocked(it);
}

void KeyValueStore::update_access_time_unlocked(const std::string& key) {
    auto it = store.find(key);
    if (it != store.end()) {
        it->second.last_access_time = std::chrono::system_clock::now();
    }
}

void KeyValueStore::erase_unlocked(StoreMap::iterator it) {
    const std::string key = it->first;
    store.erase(it);
    if (lru_cache) lru_cache->remove(key);
}

void KeyValueStore::touch_unlocked(const std::string& key) {
    update_access_time_unlocked(key);
    if (lru_cache) lru_cache->touch(key);
}

// ===== STRING =====

Result<std::string> KeyValueStore::set(const std::string& key,
                                       const std::string& value,
                                       std::optional<int> ttl_seconds) {
    std::lock_guard<std::mutex> lock(store_mutex);

    // SET always replaces the value and the TTL, matching Redis semantics.
    store[key] = KeyMetadata(StringValue(value), ttl_seconds);
    touch_unlocked(key);
    return std::string("OK");
}

Result<std::optional<std::string>> KeyValueStore::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return std::optional<std::string>{};

    const auto* sv = std::get_if<StringValue>(&it->second.data);
    if (!sv) return make_error<std::optional<std::string>>(WRONGTYPE_ERROR);

    touch_unlocked(key);
    return std::optional<std::string>{sv->content};
}

Result<int> KeyValueStore::del(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(store_mutex);

    int removed = 0;
    for (const auto& key : keys) {
        auto it = find_live_unlocked(key);
        if (it == store.end()) continue;
        erase_unlocked(it);
        ++removed;
    }
    return removed;
}

Result<int> KeyValueStore::exists(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(store_mutex);

    int found = 0;
    for (const auto& key : keys) {
        if (key_exists_unlocked(key)) ++found;
    }
    return found;
}

Result<int64_t> KeyValueStore::incr(const std::string& key) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) {
        // Missing key behaves like "0", so INCR creates it at 1.
        store[key] = KeyMetadata(StringValue("1"));
        touch_unlocked(key);
        return int64_t{1};
    }

    auto* sv = std::get_if<StringValue>(&it->second.data);
    if (!sv) return make_error<int64_t>(WRONGTYPE_ERROR);

    const auto current = parse_int64(sv->content);
    if (!current) return make_error<int64_t>(NOT_INTEGER_ERROR);
    if (*current == INT64_MAX) return make_error<int64_t>(NOT_INTEGER_ERROR);

    const int64_t next = *current + 1;
    sv->content = std::to_string(next);  // TTL is deliberately preserved
    touch_unlocked(key);
    return next;
}

Result<int64_t> KeyValueStore::decr(const std::string& key) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) {
        store[key] = KeyMetadata(StringValue("-1"));
        touch_unlocked(key);
        return int64_t{-1};
    }

    auto* sv = std::get_if<StringValue>(&it->second.data);
    if (!sv) return make_error<int64_t>(WRONGTYPE_ERROR);

    const auto current = parse_int64(sv->content);
    if (!current) return make_error<int64_t>(NOT_INTEGER_ERROR);
    if (*current == INT64_MIN) return make_error<int64_t>(NOT_INTEGER_ERROR);

    const int64_t next = *current - 1;
    sv->content = std::to_string(next);
    touch_unlocked(key);
    return next;
}

// ===== HASH =====

Result<int> KeyValueStore::hset(
    const std::string& key, const std::map<std::string, std::string>& fields) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) {
        it = store.emplace(key, KeyMetadata(HashValue{})).first;
    }

    auto* hv = std::get_if<HashValue>(&it->second.data);
    if (!hv) return make_error<int>(WRONGTYPE_ERROR);

    // Returns the number of *new* fields, like Redis; updates count as 0.
    int added = 0;
    for (const auto& [field, value] : fields) {
        if (hv->fields.find(field) == hv->fields.end()) ++added;
        hv->fields[field] = value;
    }

    touch_unlocked(key);
    return added;
}

Result<std::optional<std::string>> KeyValueStore::hget(
    const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return std::optional<std::string>{};

    const auto* hv = std::get_if<HashValue>(&it->second.data);
    if (!hv) return make_error<std::optional<std::string>>(WRONGTYPE_ERROR);

    touch_unlocked(key);
    auto field_it = hv->fields.find(field);
    if (field_it == hv->fields.end()) return std::optional<std::string>{};
    return std::optional<std::string>{field_it->second};
}

Result<int> KeyValueStore::hdel(const std::string& key,
                                const std::vector<std::string>& fields) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return 0;

    auto* hv = std::get_if<HashValue>(&it->second.data);
    if (!hv) return make_error<int>(WRONGTYPE_ERROR);

    int removed = 0;
    for (const auto& field : fields) removed += static_cast<int>(hv->fields.erase(field));

    // An empty hash is not a thing in Redis: the key disappears with it.
    if (hv->fields.empty()) {
        erase_unlocked(it);
    } else {
        touch_unlocked(key);
    }
    return removed;
}

Result<int> KeyValueStore::hexists(const std::string& key,
                                   const std::string& field) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return 0;

    const auto* hv = std::get_if<HashValue>(&it->second.data);
    if (!hv) return make_error<int>(WRONGTYPE_ERROR);

    touch_unlocked(key);
    return hv->fields.count(field) ? 1 : 0;
}

Result<std::map<std::string, std::string>> KeyValueStore::hgetall(
    const std::string& key) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return std::map<std::string, std::string>{};

    const auto* hv = std::get_if<HashValue>(&it->second.data);
    if (!hv) {
        return make_error<std::map<std::string, std::string>>(WRONGTYPE_ERROR);
    }

    touch_unlocked(key);
    return hv->fields;
}

// ===== LIST =====

Result<int> KeyValueStore::lpush(const std::string& key,
                                 const std::vector<std::string>& elements) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) {
        it = store.emplace(key, KeyMetadata(ListValue{})).first;
    }

    auto* lv = std::get_if<ListValue>(&it->second.data);
    if (!lv) return make_error<int>(WRONGTYPE_ERROR);

    // Each element goes to the head in turn, so the last one pushed ends up first.
    for (const auto& element : elements) lv->elements.push_front(element);

    touch_unlocked(key);
    return static_cast<int>(lv->elements.size());
}

Result<int> KeyValueStore::rpush(const std::string& key,
                                 const std::vector<std::string>& elements) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) {
        it = store.emplace(key, KeyMetadata(ListValue{})).first;
    }

    auto* lv = std::get_if<ListValue>(&it->second.data);
    if (!lv) return make_error<int>(WRONGTYPE_ERROR);

    for (const auto& element : elements) lv->elements.push_back(element);

    touch_unlocked(key);
    return static_cast<int>(lv->elements.size());
}

Result<std::vector<std::string>> KeyValueStore::lpop(const std::string& key,
                                                     int count) {
    std::lock_guard<std::mutex> lock(store_mutex);

    if (count < 0) {
        return make_error<std::vector<std::string>>("count must not be negative");
    }

    auto it = find_live_unlocked(key);
    if (it == store.end()) return std::vector<std::string>{};

    auto* lv = std::get_if<ListValue>(&it->second.data);
    if (!lv) return make_error<std::vector<std::string>>(WRONGTYPE_ERROR);

    std::vector<std::string> popped;
    for (int i = 0; i < count && !lv->elements.empty(); ++i) {
        popped.push_back(lv->elements.front());
        lv->elements.pop_front();
    }

    if (lv->elements.empty()) {
        erase_unlocked(it);
    } else {
        touch_unlocked(key);
    }
    return popped;
}

Result<std::vector<std::string>> KeyValueStore::rpop(const std::string& key,
                                                     int count) {
    std::lock_guard<std::mutex> lock(store_mutex);

    if (count < 0) {
        return make_error<std::vector<std::string>>("count must not be negative");
    }

    auto it = find_live_unlocked(key);
    if (it == store.end()) return std::vector<std::string>{};

    auto* lv = std::get_if<ListValue>(&it->second.data);
    if (!lv) return make_error<std::vector<std::string>>(WRONGTYPE_ERROR);

    std::vector<std::string> popped;
    for (int i = 0; i < count && !lv->elements.empty(); ++i) {
        popped.push_back(lv->elements.back());
        lv->elements.pop_back();
    }

    if (lv->elements.empty()) {
        erase_unlocked(it);
    } else {
        touch_unlocked(key);
    }
    return popped;
}

Result<int> KeyValueStore::llen(const std::string& key) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return 0;

    const auto* lv = std::get_if<ListValue>(&it->second.data);
    if (!lv) return make_error<int>(WRONGTYPE_ERROR);

    touch_unlocked(key);
    return static_cast<int>(lv->elements.size());
}

Result<std::vector<std::string>> KeyValueStore::lrange(const std::string& key,
                                                       int start, int stop) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return std::vector<std::string>{};

    const auto* lv = std::get_if<ListValue>(&it->second.data);
    if (!lv) return make_error<std::vector<std::string>>(WRONGTYPE_ERROR);

    touch_unlocked(key);

    // Redis index rules: inclusive range, negatives count back from the end.
    const int len = static_cast<int>(lv->elements.size());
    if (start < 0) start += len;
    if (stop < 0) stop += len;
    if (start < 0) start = 0;
    if (stop >= len) stop = len - 1;
    if (start > stop || start >= len) return std::vector<std::string>{};

    std::vector<std::string> range;
    range.reserve(static_cast<size_t>(stop - start + 1));
    for (int i = start; i <= stop; ++i) {
        range.push_back(lv->elements[static_cast<size_t>(i)]);
    }
    return range;
}

// ===== SET =====

Result<int> KeyValueStore::sadd(const std::string& key,
                                const std::vector<std::string>& members) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) {
        it = store.emplace(key, KeyMetadata(SetValue{})).first;
    }

    auto* sv = std::get_if<SetValue>(&it->second.data);
    if (!sv) return make_error<int>(WRONGTYPE_ERROR);

    int added = 0;
    for (const auto& member : members) {
        if (sv->members.insert(member).second) ++added;
    }

    touch_unlocked(key);
    return added;
}

Result<int> KeyValueStore::srem(const std::string& key,
                                const std::vector<std::string>& members) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return 0;

    auto* sv = std::get_if<SetValue>(&it->second.data);
    if (!sv) return make_error<int>(WRONGTYPE_ERROR);

    int removed = 0;
    for (const auto& member : members) {
        removed += static_cast<int>(sv->members.erase(member));
    }

    if (sv->members.empty()) {
        erase_unlocked(it);
    } else {
        touch_unlocked(key);
    }
    return removed;
}

Result<std::vector<std::string>> KeyValueStore::smembers(const std::string& key) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return std::vector<std::string>{};

    const auto* sv = std::get_if<SetValue>(&it->second.data);
    if (!sv) return make_error<std::vector<std::string>>(WRONGTYPE_ERROR);

    touch_unlocked(key);

    // Sorted so responses are deterministic; unordered_set order is not.
    std::vector<std::string> members(sv->members.begin(), sv->members.end());
    std::sort(members.begin(), members.end());
    return members;
}

Result<int> KeyValueStore::sismember(const std::string& key,
                                     const std::string& member) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return 0;

    const auto* sv = std::get_if<SetValue>(&it->second.data);
    if (!sv) return make_error<int>(WRONGTYPE_ERROR);

    touch_unlocked(key);
    return sv->members.count(member) ? 1 : 0;
}

Result<std::vector<std::string>> KeyValueStore::sinter(
    const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(store_mutex);

    if (keys.empty()) return std::vector<std::string>{};

    std::optional<std::unordered_set<std::string>> acc;
    for (const auto& key : keys) {
        auto it = find_live_unlocked(key);
        if (it == store.end()) return std::vector<std::string>{};  // empty set wins

        const auto* sv = std::get_if<SetValue>(&it->second.data);
        if (!sv) return make_error<std::vector<std::string>>(WRONGTYPE_ERROR);
        touch_unlocked(key);

        if (!acc) {
            acc = sv->members;
            continue;
        }
        std::unordered_set<std::string> next;
        for (const auto& m : *acc) {
            if (sv->members.count(m)) next.insert(m);
        }
        acc = std::move(next);
        if (acc->empty()) break;
    }

    std::vector<std::string> result(acc->begin(), acc->end());
    std::sort(result.begin(), result.end());
    return result;
}

Result<std::vector<std::string>> KeyValueStore::sunion(
    const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(store_mutex);

    std::unordered_set<std::string> acc;
    for (const auto& key : keys) {
        auto it = find_live_unlocked(key);
        if (it == store.end()) continue;  // missing key contributes nothing

        const auto* sv = std::get_if<SetValue>(&it->second.data);
        if (!sv) return make_error<std::vector<std::string>>(WRONGTYPE_ERROR);
        touch_unlocked(key);

        acc.insert(sv->members.begin(), sv->members.end());
    }

    std::vector<std::string> result(acc.begin(), acc.end());
    std::sort(result.begin(), result.end());
    return result;
}

// ===== TTL =====

Result<int> KeyValueStore::expire(const std::string& key, int seconds) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return 0;

    if (seconds <= 0) {
        // Redis deletes the key outright for a non-positive TTL.
        erase_unlocked(it);
        return 1;
    }

    it->second.expiry_time =
        std::chrono::system_clock::now() + std::chrono::seconds(seconds);
    touch_unlocked(key);
    return 1;
}

Result<int> KeyValueStore::ttl(const std::string& key) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return -2;              // no such key
    if (!it->second.expiry_time) return -1;        // key exists, no expiry

    const auto remaining = *it->second.expiry_time - std::chrono::system_clock::now();
    const auto secs =
        std::chrono::duration_cast<std::chrono::duration<double>>(remaining).count();
    // Round up: a key set with EX 10 should report 10, not 9.
    return std::max(0, static_cast<int>(std::ceil(secs)));
}

// ===== Introspection =====

size_t KeyValueStore::size() const {
    std::lock_guard<std::mutex> lock(store_mutex);
    return store.size();
}

std::vector<std::string> KeyValueStore::keys() const {
    std::lock_guard<std::mutex> lock(store_mutex);

    std::vector<std::string> result;
    result.reserve(store.size());
    for (const auto& [key, meta] : store) {
        if (!meta.is_expired()) result.push_back(key);
    }
    return result;
}

Result<std::string> KeyValueStore::type(const std::string& key) {
    std::lock_guard<std::mutex> lock(store_mutex);

    auto it = find_live_unlocked(key);
    if (it == store.end()) return std::string("none");
    return value_type_name(it->second.data);
}

}  // namespace flashkv
