#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "storage/key_value_store.hpp"

namespace flashkv::test {

class TTLTest : public ::testing::Test {
protected:
    KeyValueStore store;
};

TEST_F(TTLTest, ExpireAndGet) {
    store.set("key", "value", 1);  // 1 second TTL
    auto result1 = store.get("key");
    EXPECT_TRUE(get_value(result1).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto result2 = store.get("key");
    EXPECT_FALSE(get_value(result2).has_value());
}

TEST_F(TTLTest, TTLCommand) {
    store.set("key", "value", 10);
    auto result = store.ttl("key");
    ASSERT_FALSE(is_error(result));
    int ttl_val = get_value(result);
    EXPECT_TRUE(ttl_val > 0 && ttl_val <= 10);
}

TEST_F(TTLTest, NoTTLInitially) {
    store.set("key", "value");
    auto result = store.ttl("key");
    EXPECT_EQ(get_value(result), -1);
}

TEST_F(TTLTest, KeyNotFoundTTL) {
    auto result = store.ttl("nonexistent");
    EXPECT_EQ(get_value(result), -2);
}

TEST_F(TTLTest, ExpiredKeyNotInExists) {
    store.set("key", "value", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto result = store.exists({"key"});
    EXPECT_EQ(get_value(result), 0);
}

TEST_F(TTLTest, ExpireCommandSetsTTLOnExistingKey) {
    store.set("key", "value");
    auto result = store.expire("key", 30);
    EXPECT_EQ(get_value(result), 1);

    int remaining = get_value(store.ttl("key"));
    EXPECT_TRUE(remaining > 0 && remaining <= 30);
}

TEST_F(TTLTest, ExpireOnMissingKeyReturnsZero) {
    auto result = store.expire("nope", 30);
    EXPECT_EQ(get_value(result), 0);
}

TEST_F(TTLTest, ExpireWithNonPositiveTTLDeletesKey) {
    store.set("key", "value");
    auto result = store.expire("key", 0);
    EXPECT_EQ(get_value(result), 1);
    EXPECT_FALSE(get_value(store.get("key")).has_value());
}

TEST_F(TTLTest, PlainSetClearsPreviousTTL) {
    store.set("key", "value", 30);
    store.set("key", "value2");
    EXPECT_EQ(get_value(store.ttl("key")), -1);
}

TEST_F(TTLTest, IncrPreservesTTL) {
    store.set("counter", "1", 30);
    store.incr("counter");
    int remaining = get_value(store.ttl("counter"));
    EXPECT_TRUE(remaining > 0 && remaining <= 30);
}

TEST_F(TTLTest, ExpiryAppliesToEveryDataType) {
    store.lpush("list", {"a"});
    store.expire("list", 1);
    store.sadd("set", {"m"});
    store.expire("set", 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_EQ(get_value(store.llen("list")), 0);
    EXPECT_TRUE(get_value(store.smembers("set")).empty());
}

}  // namespace flashkv::test
