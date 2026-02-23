#include <gtest/gtest.h>

#include "storage/key_value_store.hpp"

namespace flashkv::test {

class StorageEngineTest : public ::testing::Test {
protected:
    KeyValueStore store;
};

// ===== STRING =====

TEST_F(StorageEngineTest, SetAndGetString) {
    auto set_result = store.set("key", "value");
    ASSERT_FALSE(is_error(set_result));

    auto get_result = store.get("key");
    ASSERT_FALSE(is_error(get_result));
    auto val = get_value(get_result);
    EXPECT_EQ(val, "value");
}

TEST_F(StorageEngineTest, SetOverwritesExistingValue) {
    store.set("key", "first");
    store.set("key", "second");
    auto val = get_value(store.get("key"));
    EXPECT_EQ(val, "second");
}

TEST_F(StorageEngineTest, GetNonExistentKey) {
    auto result = store.get("nonexistent");
    ASSERT_FALSE(is_error(result));
    auto val = get_value(result);
    EXPECT_FALSE(val.has_value());
}

TEST_F(StorageEngineTest, DeleteKey) {
    store.set("key", "value");
    auto result = store.del({"key"});
    ASSERT_FALSE(is_error(result));
    EXPECT_EQ(get_value(result), 1);

    auto get_result = store.get("key");
    auto val = get_value(get_result);
    EXPECT_FALSE(val.has_value());
}

TEST_F(StorageEngineTest, DeleteMultipleKeys) {
    store.set("k1", "v1");
    store.set("k2", "v2");
    auto result = store.del({"k1", "k2", "k3"});
    EXPECT_EQ(get_value(result), 2);
}

TEST_F(StorageEngineTest, ExistsKey) {
    store.set("key", "value");
    auto result = store.exists({"key", "nonexistent"});
    EXPECT_EQ(get_value(result), 1);
}

TEST_F(StorageEngineTest, IncrementString) {
    store.set("counter", "10");
    auto result = store.incr("counter");
    ASSERT_FALSE(is_error(result));
    EXPECT_EQ(get_value(result), 11);
}

TEST_F(StorageEngineTest, DecrementString) {
    store.set("counter", "10");
    auto result = store.decr("counter");
    ASSERT_FALSE(is_error(result));
    EXPECT_EQ(get_value(result), 9);
}

TEST_F(StorageEngineTest, IncrCreatesMissingKeyAtOne) {
    auto result = store.incr("fresh");
    ASSERT_FALSE(is_error(result));
    EXPECT_EQ(get_value(result), 1);
    EXPECT_EQ(get_value(store.get("fresh")), "1");
}

TEST_F(StorageEngineTest, DecrCreatesMissingKeyAtMinusOne) {
    auto result = store.decr("fresh");
    ASSERT_FALSE(is_error(result));
    EXPECT_EQ(get_value(result), -1);
}

TEST_F(StorageEngineTest, IncrOnNonNumericValue) {
    store.set("notnum", "abc");
    auto result = store.incr("notnum");
    EXPECT_TRUE(is_error(result));
}

TEST_F(StorageEngineTest, IncrRejectsTrailingGarbage) {
    store.set("notnum", "12abc");
    auto result = store.incr("notnum");
    EXPECT_TRUE(is_error(result));
}

// ===== HASH =====

TEST_F(StorageEngineTest, HSetAndHGet) {
    std::map<std::string, std::string> fields = {{"f1", "v1"}};
    auto set_result = store.hset("hash", fields);
    ASSERT_FALSE(is_error(set_result));
    EXPECT_EQ(get_value(set_result), 1);

    auto get_result = store.hget("hash", "f1");
    ASSERT_FALSE(is_error(get_result));
    EXPECT_EQ(get_value(get_result), "v1");
}

TEST_F(StorageEngineTest, HSetCountsOnlyNewFields) {
    store.hset("hash", {{"f1", "v1"}});
    auto result = store.hset("hash", {{"f1", "updated"}, {"f2", "v2"}});
    EXPECT_EQ(get_value(result), 1);  // f1 already existed
    EXPECT_EQ(get_value(store.hget("hash", "f1")), "updated");
}

TEST_F(StorageEngineTest, HGetAll) {
    std::map<std::string, std::string> fields = {{"f1", "v1"}, {"f2", "v2"}};
    store.hset("hash", fields);
    auto result = store.hgetall("hash");
    ASSERT_FALSE(is_error(result));
    auto all = get_value(result);
    EXPECT_EQ(all.size(), 2u);
}

TEST_F(StorageEngineTest, HDeleteField) {
    std::map<std::string, std::string> fields = {{"f1", "v1"}, {"f2", "v2"}};
    store.hset("hash", fields);
    auto result = store.hdel("hash", {"f1"});
    EXPECT_EQ(get_value(result), 1);
    EXPECT_FALSE(get_value(store.hget("hash", "f1")).has_value());
}

TEST_F(StorageEngineTest, HExists) {
    store.hset("hash", {{"f1", "v1"}});
    EXPECT_EQ(get_value(store.hexists("hash", "f1")), 1);
    EXPECT_EQ(get_value(store.hexists("hash", "nope")), 0);
}

TEST_F(StorageEngineTest, HGetFromNonHashKey) {
    store.set("string", "value");
    auto result = store.hget("string", "field");
    EXPECT_TRUE(is_error(result));
}

// ===== LIST =====

TEST_F(StorageEngineTest, LPushAndLPop) {
    auto push_result = store.lpush("list", {"a", "b", "c"});
    ASSERT_FALSE(is_error(push_result));
    EXPECT_EQ(get_value(push_result), 3);

    auto pop_result = store.lpop("list", 1);
    ASSERT_FALSE(is_error(pop_result));
    auto popped = get_value(pop_result);
    ASSERT_EQ(popped.size(), 1u);
    EXPECT_EQ(popped[0], "c");  // Last pushed, first popped
}

TEST_F(StorageEngineTest, RPushAndRPop) {
    store.rpush("list", {"a", "b", "c"});
    auto popped = get_value(store.rpop("list", 1));
    ASSERT_EQ(popped.size(), 1u);
    EXPECT_EQ(popped[0], "c");
}

TEST_F(StorageEngineTest, PopMoreThanAvailable) {
    store.rpush("list", {"a", "b"});
    auto popped = get_value(store.lpop("list", 10));
    EXPECT_EQ(popped.size(), 2u);
    EXPECT_EQ(get_value(store.llen("list")), 0);  // key is gone once empty
}

TEST_F(StorageEngineTest, LLen) {
    store.lpush("list", {"a", "b", "c"});
    auto result = store.llen("list");
    EXPECT_EQ(get_value(result), 3);
}

TEST_F(StorageEngineTest, LRange) {
    store.lpush("list", {"a", "b", "c"});
    auto result = store.lrange("list", 0, 1);
    ASSERT_FALSE(is_error(result));
    auto range = get_value(result);
    ASSERT_EQ(range.size(), 2u);
    EXPECT_EQ(range[0], "c");
    EXPECT_EQ(range[1], "b");
}

TEST_F(StorageEngineTest, LRangeNegativeIndices) {
    store.rpush("list", {"a", "b", "c", "d"});
    auto range = get_value(store.lrange("list", -2, -1));
    ASSERT_EQ(range.size(), 2u);
    EXPECT_EQ(range[0], "c");
    EXPECT_EQ(range[1], "d");
}

TEST_F(StorageEngineTest, LRangeOutOfBoundsIsEmpty) {
    store.rpush("list", {"a"});
    auto range = get_value(store.lrange("list", 5, 10));
    EXPECT_TRUE(range.empty());
}

TEST_F(StorageEngineTest, LPopFromNonListKey) {
    store.set("string", "value");
    auto result = store.lpop("string", 1);
    EXPECT_TRUE(is_error(result));
}

// ===== SET =====

TEST_F(StorageEngineTest, SAddAndSMembers) {
    auto add_result = store.sadd("set", {"x", "y", "z"});
    ASSERT_FALSE(is_error(add_result));
    EXPECT_EQ(get_value(add_result), 3);

    auto members_result = store.smembers("set");
    ASSERT_FALSE(is_error(members_result));
    auto members = get_value(members_result);
    EXPECT_EQ(members.size(), 3u);
}

TEST_F(StorageEngineTest, SAddIgnoresDuplicates) {
    store.sadd("set", {"x"});
    auto result = store.sadd("set", {"x", "y"});
    EXPECT_EQ(get_value(result), 1);
}

TEST_F(StorageEngineTest, SRemove) {
    store.sadd("set", {"x", "y"});
    auto result = store.srem("set", {"x"});
    EXPECT_EQ(get_value(result), 1);
}

TEST_F(StorageEngineTest, SIsMember) {
    store.sadd("set", {"x"});
    auto result = store.sismember("set", "x");
    EXPECT_EQ(get_value(result), 1);

    auto not_result = store.sismember("set", "y");
    EXPECT_EQ(get_value(not_result), 0);
}

TEST_F(StorageEngineTest, SInter) {
    store.sadd("set1", {"a", "b", "c"});
    store.sadd("set2", {"b", "c", "d"});
    auto result = store.sinter({"set1", "set2"});
    ASSERT_FALSE(is_error(result));
    auto inter = get_value(result);
    ASSERT_EQ(inter.size(), 2u);  // b, c
    EXPECT_EQ(inter[0], "b");
    EXPECT_EQ(inter[1], "c");
}

TEST_F(StorageEngineTest, SInterWithMissingKeyIsEmpty) {
    store.sadd("set1", {"a"});
    auto inter = get_value(store.sinter({"set1", "missing"}));
    EXPECT_TRUE(inter.empty());
}

TEST_F(StorageEngineTest, SUnion) {
    store.sadd("set1", {"a", "b"});
    store.sadd("set2", {"b", "c"});
    auto result = store.sunion({"set1", "set2"});
    ASSERT_FALSE(is_error(result));
    auto uni = get_value(result);
    EXPECT_EQ(uni.size(), 3u);  // a, b, c
}

// ===== Type Mismatch =====

TEST_F(StorageEngineTest, TypeMismatchGetOnHash) {
    std::map<std::string, std::string> fields = {{"f", "v"}};
    store.hset("key", fields);
    auto result = store.get("key");
    EXPECT_TRUE(is_error(result));
    EXPECT_EQ(get_error(result), WRONGTYPE_ERROR);
}

TEST_F(StorageEngineTest, TypeMismatchLPushOnString) {
    store.set("key", "value");
    auto result = store.lpush("key", {"elem"});
    EXPECT_TRUE(is_error(result));
}

TEST_F(StorageEngineTest, TypeMismatchSAddOnList) {
    store.lpush("key", {"elem"});
    auto result = store.sadd("key", {"member"});
    EXPECT_TRUE(is_error(result));
}

TEST_F(StorageEngineTest, TypeReportsStoredKind) {
    store.set("s", "v");
    store.lpush("l", {"e"});
    EXPECT_EQ(get_value(store.type("s")), "string");
    EXPECT_EQ(get_value(store.type("l")), "list");
    EXPECT_EQ(get_value(store.type("missing")), "none");
}

}  // namespace flashkv::test
