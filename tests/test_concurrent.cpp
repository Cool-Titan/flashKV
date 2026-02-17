#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "storage/key_value_store.hpp"
#include "storage/lru_cache.hpp"

namespace flashkv::test {

class ConcurrencyTest : public ::testing::Test {
protected:
    KeyValueStore store;
};

TEST_F(ConcurrencyTest, MultiThreadSetGet) {
    std::vector<std::thread> threads;

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, i]() {
            for (int j = 0; j < 100; ++j) {
                store.set("key_" + std::to_string(j), "val_" + std::to_string(i));
                auto result = store.get("key_" + std::to_string(j));
                EXPECT_FALSE(is_error(result));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
}

TEST_F(ConcurrencyTest, MultiThreadDelete) {
    // Pre-populate
    for (int i = 0; i < 100; ++i) {
        store.set("key_" + std::to_string(i), "val");
    }

    std::vector<std::thread> threads;
    std::atomic<int> deleted{0};

    // Each thread owns a disjoint range, so every key must be deleted exactly once.
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, &deleted, i]() {
            for (int j = i * 10; j < (i + 1) * 10; ++j) {
                auto result = store.del({"key_" + std::to_string(j)});
                if (!is_error(result) && get_value(result) == 1) ++deleted;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(deleted.load(), 100);
}

TEST_F(ConcurrencyTest, MultiThreadMixedOps) {
    std::vector<std::thread> threads;

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, i]() {
            for (int j = 0; j < 100; ++j) {
                int op = j % 3;
                std::string key = "key_" + std::to_string(i);

                if (op == 0) {
                    store.set(key, "val");
                } else if (op == 1) {
                    store.get(key);
                } else {
                    store.del({key});
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
}

TEST_F(ConcurrencyTest, ConcurrentHashOps) {
    std::vector<std::thread> threads;

    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([this, i]() {
            std::map<std::string, std::string> fields;
            fields["f_" + std::to_string(i)] = "v_" + std::to_string(i);
            store.hset("hash", fields);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto result = store.hgetall("hash");
    ASSERT_FALSE(is_error(result));
    EXPECT_EQ(get_value(result).size(), 5u);
}

TEST_F(ConcurrencyTest, ConcurrentListOps) {
    std::vector<std::thread> threads;
    std::atomic<int> total_pushed{0};

    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([this, i, &total_pushed]() {
            auto result = store.lpush("list", {"elem_" + std::to_string(i)});
            if (!is_error(result)) ++total_pushed;
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto len_result = store.llen("list");
    EXPECT_EQ(get_value(len_result), total_pushed.load());
}

TEST_F(ConcurrencyTest, ConcurrentIncrementsAreAtomic) {
    store.set("counter", "0");

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this]() {
            for (int j = 0; j < 100; ++j) store.incr("counter");
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    // Read-modify-write happens under the store mutex: no lost updates.
    EXPECT_EQ(get_value(store.get("counter")), "1000");
}

TEST_F(ConcurrencyTest, ConcurrentSetAddsAreDeduplicated) {
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([this]() {
            for (int j = 0; j < 50; ++j) {
                store.sadd("set", {"m_" + std::to_string(j)});
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(get_value(store.smembers("set")).size(), 50u);
}

TEST_F(ConcurrencyTest, LRUStaysConsistentUnderLoad) {
    LRUCache lru(&store);
    store.set_lru_cache(&lru);

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([this, i]() {
            for (int j = 0; j < 100; ++j) {
                store.set("k_" + std::to_string((i * 100 + j) % 40), "v");
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    // Exactly the live keys are tracked, with no duplicates in the order list.
    EXPECT_EQ(lru.size(), store.size());
    auto order = lru.get_order();
    std::set<std::string> unique(order.begin(), order.end());
    EXPECT_EQ(unique.size(), order.size());
}

}  // namespace flashkv::test
