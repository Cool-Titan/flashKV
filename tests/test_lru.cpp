#include <gtest/gtest.h>

#include "storage/key_value_store.hpp"
#include "storage/lru_cache.hpp"

namespace flashkv::test {

class LRUTest : public ::testing::Test {
protected:
    KeyValueStore store;
    LRUCache lru{&store};

    void SetUp() override { store.set_lru_cache(&lru); }
};

TEST_F(LRUTest, TouchMovesToEnd) {
    lru.touch("a");
    lru.touch("b");
    lru.touch("c");
    lru.touch("b");  // Move b to end

    auto order = lru.get_order();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order.back(), "b");
    EXPECT_EQ(order[0], "a");
}

TEST_F(LRUTest, LRUKeyIsFirst) {
    lru.touch("a");
    lru.touch("b");
    lru.touch("c");

    auto lru_key = lru.get_lru_key();
    EXPECT_EQ(lru_key, "a");
}

TEST_F(LRUTest, MRUKeyIsLast) {
    lru.touch("a");
    lru.touch("b");
    lru.touch("c");

    auto mru_key = lru.get_mru_key();
    EXPECT_EQ(mru_key, "c");
}

TEST_F(LRUTest, RemoveKey) {
    lru.touch("a");
    lru.touch("b");
    lru.remove("a");

    EXPECT_EQ(lru.size(), 1u);
    auto order = lru.get_order();
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], "b");
}

TEST_F(LRUTest, GetOrder) {
    lru.touch("a");
    lru.touch("b");
    lru.touch("c");

    auto order = lru.get_order();
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "a");
    EXPECT_EQ(order[1], "b");
    EXPECT_EQ(order[2], "c");
}

TEST_F(LRUTest, EmptyCacheHasNoKeys) {
    EXPECT_EQ(lru.size(), 0u);
    EXPECT_FALSE(lru.get_lru_key().has_value());
    EXPECT_FALSE(lru.get_mru_key().has_value());
}

TEST_F(LRUTest, TouchIsIdempotentOnSize) {
    lru.touch("a");
    lru.touch("a");
    lru.touch("a");
    EXPECT_EQ(lru.size(), 1u);
}

TEST_F(LRUTest, RemovingUnknownKeyIsNoOp) {
    lru.touch("a");
    lru.remove("ghost");
    EXPECT_EQ(lru.size(), 1u);
}

TEST_F(LRUTest, StoreReadPromotesKey) {
    store.set("a", "1");
    store.set("b", "2");
    store.get("a");  // reading "a" makes it most recent

    EXPECT_EQ(lru.get_mru_key(), "a");
    EXPECT_EQ(lru.get_lru_key(), "b");
}

TEST_F(LRUTest, DeleteStopsTracking) {
    store.set("a", "1");
    store.set("b", "2");
    store.del({"a"});

    EXPECT_EQ(lru.size(), 1u);
    EXPECT_EQ(lru.get_lru_key(), "b");
}

}  // namespace flashkv::test
