#include <gtest/gtest.h>
#include <dam/storage/btree.hpp>
#include <dam/storage/buffer_pool.hpp>
#include <dam/storage/disk_manager.hpp>
#include <filesystem>

using namespace dam;
namespace fs = std::filesystem;

class BPlusTreeTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "btree_test";
        fs::remove_all(test_dir_);
        fs::create_directories(test_dir_);

        disk_manager_ = std::make_unique<DiskManager>(test_dir_ / "test.db");
        buffer_pool_ = std::make_unique<BufferPool>(100, disk_manager_.get());
    }

    void TearDown() override {
        buffer_pool_.reset();
        disk_manager_.reset();
        fs::remove_all(test_dir_);
    }

    fs::path test_dir_;
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPool> buffer_pool_;
};

TEST_F(BPlusTreeTest, InsertAndFind) {
    BPlusTree tree(buffer_pool_.get());

    EXPECT_TRUE(tree.insert("key1", "value1"));
    EXPECT_TRUE(tree.insert("key2", "value2"));
    EXPECT_TRUE(tree.insert("key3", "value3"));

    auto v1 = tree.find("key1");
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, "value1");

    auto v2 = tree.find("key2");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, "value2");

    EXPECT_FALSE(tree.find("nonexistent").has_value());
}

TEST_F(BPlusTreeTest, DuplicateKeyFails) {
    BPlusTree tree(buffer_pool_.get());

    EXPECT_TRUE(tree.insert("key", "value1"));
    EXPECT_FALSE(tree.insert("key", "value2"));

    auto v = tree.find("key");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "value1");
}

TEST_F(BPlusTreeTest, Update) {
    BPlusTree tree(buffer_pool_.get());

    tree.insert("key", "old_value");
    EXPECT_TRUE(tree.update("key", "new_value"));

    auto v = tree.find("key");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "new_value");
}

TEST_F(BPlusTreeTest, Remove) {
    BPlusTree tree(buffer_pool_.get());

    tree.insert("key1", "value1");
    tree.insert("key2", "value2");
    tree.insert("key3", "value3");

    EXPECT_TRUE(tree.remove("key2"));
    EXPECT_FALSE(tree.find("key2").has_value());
    EXPECT_TRUE(tree.find("key1").has_value());
    EXPECT_TRUE(tree.find("key3").has_value());
}

TEST_F(BPlusTreeTest, Contains) {
    BPlusTree tree(buffer_pool_.get());

    tree.insert("key", "value");

    EXPECT_TRUE(tree.contains("key"));
    EXPECT_FALSE(tree.contains("other"));
}

TEST_F(BPlusTreeTest, LargeInserts) {
    BPlusTree tree(buffer_pool_.get());

    for (int i = 0; i < 100; ++i) {
        std::string key = "key" + std::to_string(i);
        std::string value = "value" + std::to_string(i);
        EXPECT_TRUE(tree.insert(key, value));
    }

    EXPECT_EQ(tree.size(), 100);

    for (int i = 0; i < 100; ++i) {
        std::string key = "key" + std::to_string(i);
        std::string expected = "value" + std::to_string(i);
        auto v = tree.find(key);
        ASSERT_TRUE(v.has_value()) << "Key not found: " << key;
        EXPECT_EQ(*v, expected);
    }
}
