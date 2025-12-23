#include <gtest/gtest.h>
#include <dam/storage/buffer_pool.hpp>
#include <dam/storage/disk_manager.hpp>
#include <filesystem>

using namespace dam;
namespace fs = std::filesystem;

class BufferPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "buffer_pool_test";
        fs::remove_all(test_dir_);
        fs::create_directories(test_dir_);

        disk_manager_ = std::make_unique<DiskManager>(test_dir_ / "test.db");
        buffer_pool_ = std::make_unique<BufferPool>(10, disk_manager_.get());
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

TEST_F(BufferPoolTest, NewPage) {
    Page* page = buffer_pool_->new_page();
    ASSERT_NE(page, nullptr);
    EXPECT_NE(page->get_page_id(), INVALID_PAGE_ID);

    buffer_pool_->unpin_page(page->get_page_id(), false);
}

TEST_F(BufferPoolTest, FetchPage) {
    Page* page = buffer_pool_->new_page();
    ASSERT_NE(page, nullptr);
    PageId page_id = page->get_page_id();

    // Write some data
    page->get_data()[0] = 42;
    buffer_pool_->unpin_page(page_id, true);

    // Fetch it back
    Page* fetched = buffer_pool_->fetch_page(page_id);
    ASSERT_NE(fetched, nullptr);
    EXPECT_EQ(fetched->get_data()[0], 42);

    buffer_pool_->unpin_page(page_id, false);
}

TEST_F(BufferPoolTest, PinCount) {
    Page* page = buffer_pool_->new_page();
    PageId page_id = page->get_page_id();

    EXPECT_EQ(buffer_pool_->get_pin_count(page_id), 1);

    // Fetch again increases pin count
    buffer_pool_->fetch_page(page_id);
    EXPECT_EQ(buffer_pool_->get_pin_count(page_id), 2);

    // Unpin decreases
    buffer_pool_->unpin_page(page_id, false);
    EXPECT_EQ(buffer_pool_->get_pin_count(page_id), 1);

    buffer_pool_->unpin_page(page_id, false);
    EXPECT_EQ(buffer_pool_->get_pin_count(page_id), 0);
}

TEST_F(BufferPoolTest, FlushPage) {
    Page* page = buffer_pool_->new_page();
    PageId page_id = page->get_page_id();

    page->get_data()[0] = 99;
    buffer_pool_->mark_dirty(page_id);

    auto result = buffer_pool_->flush_page(page_id);
    EXPECT_TRUE(result.ok());

    buffer_pool_->unpin_page(page_id, false);
}

TEST_F(BufferPoolTest, ContainsPage) {
    Page* page = buffer_pool_->new_page();
    PageId page_id = page->get_page_id();

    EXPECT_TRUE(buffer_pool_->contains_page(page_id));
    EXPECT_FALSE(buffer_pool_->contains_page(9999));

    buffer_pool_->unpin_page(page_id, false);
}

TEST_F(BufferPoolTest, MultiplePages) {
    std::vector<PageId> page_ids;

    for (int i = 0; i < 5; ++i) {
        Page* page = buffer_pool_->new_page();
        ASSERT_NE(page, nullptr);
        page_ids.push_back(page->get_page_id());
        buffer_pool_->unpin_page(page->get_page_id(), false);
    }

    for (PageId id : page_ids) {
        EXPECT_TRUE(buffer_pool_->contains_page(id));
    }
}
