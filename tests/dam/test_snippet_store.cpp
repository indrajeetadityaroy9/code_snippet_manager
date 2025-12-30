#include <gtest/gtest.h>
#include <dam/dam.hpp>
#include <filesystem>

using namespace dam;
namespace fs = std::filesystem;

class SnippetStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "dam_test";
        fs::remove_all(test_dir_);
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::unique_ptr<SnippetStore> open_store() {
        Config config;
        config.root_directory = test_dir_;
        config.buffer_pool_size = 100;
        auto result = SnippetStore::open(config);
        EXPECT_TRUE(result.ok()) << result.error().to_string();
        return std::move(result.value());
    }

    fs::path test_dir_;
};

// ============================================================================
// Basic Operations
// ============================================================================

TEST_F(SnippetStoreTest, OpenAndClose) {
    auto store = open_store();
    EXPECT_TRUE(store->is_open());
    EXPECT_EQ(store->count(), 0);

    store->close();
    EXPECT_FALSE(store->is_open());
}

TEST_F(SnippetStoreTest, AddSnippet) {
    auto store = open_store();

    auto result = store->add("echo 'hello'", "hello-script", {"bash", "example"});
    ASSERT_TRUE(result.ok()) << result.error().to_string();

    SnippetId id = result.value();
    EXPECT_NE(id, INVALID_SNIPPET_ID);
    EXPECT_EQ(store->count(), 1);
}

TEST_F(SnippetStoreTest, GetSnippet) {
    auto store = open_store();

    auto add_result = store->add("print('hello')", "py-hello", {"python"}, "python");
    ASSERT_TRUE(add_result.ok());
    SnippetId id = add_result.value();

    auto snippet = store->get(id);
    ASSERT_TRUE(snippet.ok());
    EXPECT_EQ(snippet.value().name, "py-hello");
    EXPECT_EQ(snippet.value().content, "print('hello')");
    EXPECT_EQ(snippet.value().language, "python");
    ASSERT_EQ(snippet.value().tags.size(), 1);
    EXPECT_EQ(snippet.value().tags[0], "python");
}

TEST_F(SnippetStoreTest, FindByName) {
    auto store = open_store();

    store->add("content1", "snippet-one", {});
    store->add("content2", "snippet-two", {});

    auto id = store->find_by_name("snippet-two");
    ASSERT_TRUE(id.ok());

    auto snippet = store->get(id.value());
    ASSERT_TRUE(snippet.ok());
    EXPECT_EQ(snippet.value().content, "content2");
}

TEST_F(SnippetStoreTest, RemoveSnippet) {
    auto store = open_store();

    auto add_result = store->add("content", "to-remove", {"tag1"});
    ASSERT_TRUE(add_result.ok());
    SnippetId id = add_result.value();

    EXPECT_EQ(store->count(), 1);

    auto remove_result = store->remove(id);
    ASSERT_TRUE(remove_result.ok());

    EXPECT_EQ(store->count(), 0);
    EXPECT_FALSE(store->get(id).ok());
}

// ============================================================================
// Tag Operations
// ============================================================================

TEST_F(SnippetStoreTest, AddTag) {
    auto store = open_store();

    auto add_result = store->add("content", "snippet", {"initial"});
    ASSERT_TRUE(add_result.ok());
    SnippetId id = add_result.value();

    auto tag_result = store->add_tag(id, "new-tag");
    ASSERT_TRUE(tag_result.ok());

    auto snippet = store->get(id);
    ASSERT_TRUE(snippet.ok());
    EXPECT_EQ(snippet.value().tags.size(), 2);
}

TEST_F(SnippetStoreTest, RemoveTag) {
    auto store = open_store();

    auto add_result = store->add("content", "snippet", {"tag1", "tag2"});
    ASSERT_TRUE(add_result.ok());
    SnippetId id = add_result.value();

    auto tag_result = store->remove_tag(id, "tag1");
    ASSERT_TRUE(tag_result.ok());

    auto snippet = store->get(id);
    ASSERT_TRUE(snippet.ok());
    ASSERT_EQ(snippet.value().tags.size(), 1);
    EXPECT_EQ(snippet.value().tags[0], "tag2");
}

TEST_F(SnippetStoreTest, GetAllTags) {
    auto store = open_store();

    store->add("c1", "s1", {"bash", "utils"});
    store->add("c2", "s2", {"python", "utils"});
    store->add("c3", "s3", {"bash", "api"});

    auto tags = store->get_all_tags();
    ASSERT_TRUE(tags.ok());
    EXPECT_EQ(tags.value().size(), 4);  // bash, utils, python, api
}

TEST_F(SnippetStoreTest, GetTagCounts) {
    auto store = open_store();

    store->add("c1", "s1", {"bash", "utils"});
    store->add("c2", "s2", {"python", "utils"});
    store->add("c3", "s3", {"bash"});

    auto counts = store->get_tag_counts();
    ASSERT_TRUE(counts.ok());
    EXPECT_EQ(counts.value()["bash"], 2);
    EXPECT_EQ(counts.value()["utils"], 2);
    EXPECT_EQ(counts.value()["python"], 1);
}

// ============================================================================
// Query Operations
// ============================================================================

TEST_F(SnippetStoreTest, ListAll) {
    auto store = open_store();

    store->add("c1", "s1", {});
    store->add("c2", "s2", {});
    store->add("c3", "s3", {});

    auto all = store->list_all();
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all.value().size(), 3);
}

TEST_F(SnippetStoreTest, FindByTag) {
    auto store = open_store();

    store->add("bash content", "bash-script", {"bash"});
    store->add("python content", "py-script", {"python"});
    store->add("another bash", "bash2", {"bash", "utils"});

    auto bash_snippets = store->find_by_tag("bash");
    ASSERT_TRUE(bash_snippets.ok());
    EXPECT_EQ(bash_snippets.value().size(), 2);

    auto python_snippets = store->find_by_tag("python");
    ASSERT_TRUE(python_snippets.ok());
    EXPECT_EQ(python_snippets.value().size(), 1);
}

TEST_F(SnippetStoreTest, FindByLanguage) {
    auto store = open_store();

    store->add("c1", "s1", {}, "bash");
    store->add("c2", "s2", {}, "python");
    store->add("c3", "s3", {}, "bash");

    auto bash_snippets = store->find_by_language("bash");
    ASSERT_TRUE(bash_snippets.ok());
    EXPECT_EQ(bash_snippets.value().size(), 2);

    auto python_snippets = store->find_by_language("python");
    ASSERT_TRUE(python_snippets.ok());
    EXPECT_EQ(python_snippets.value().size(), 1);
}

// ============================================================================
// Language Detection
// ============================================================================

TEST_F(SnippetStoreTest, LanguageDetectionFromShebang) {
    auto store = open_store();

    auto result = store->add("#!/bin/bash\necho hello", "bash-script", {});
    ASSERT_TRUE(result.ok());

    auto snippet = store->get(result.value());
    ASSERT_TRUE(snippet.ok());
    EXPECT_EQ(snippet.value().language, "bash");
}

TEST_F(SnippetStoreTest, LanguageDetectionFromEnvShebang) {
    auto store = open_store();

    auto result = store->add("#!/usr/bin/env python3\nprint('hello')", "py-script", {});
    ASSERT_TRUE(result.ok());

    auto snippet = store->get(result.value());
    ASSERT_TRUE(snippet.ok());
    EXPECT_EQ(snippet.value().language, "python");
}

TEST_F(SnippetStoreTest, LanguageDetectionFromFilename) {
    auto store = open_store();

    auto result = store->add("function test() {}", "script.js", {});
    ASSERT_TRUE(result.ok());

    auto snippet = store->get(result.value());
    ASSERT_TRUE(snippet.ok());
    EXPECT_EQ(snippet.value().language, "javascript");
}

TEST_F(SnippetStoreTest, ExplicitLanguageOverridesDetection) {
    auto store = open_store();

    // Content has bash shebang but we specify python
    auto result = store->add("#!/bin/bash\necho hello", "script", {}, "python");
    ASSERT_TRUE(result.ok());

    auto snippet = store->get(result.value());
    ASSERT_TRUE(snippet.ok());
    EXPECT_EQ(snippet.value().language, "python");
}

// ============================================================================
// Persistence
// ============================================================================

TEST_F(SnippetStoreTest, PersistenceAcrossReopen) {
    SnippetId id1, id2;

    // First session: add snippets
    {
        auto store = open_store();
        auto r1 = store->add("content1", "snippet1", {"tag1"});
        auto r2 = store->add("content2", "snippet2", {"tag2"});
        ASSERT_TRUE(r1.ok());
        ASSERT_TRUE(r2.ok());
        id1 = r1.value();
        id2 = r2.value();
        store->close();
    }

    // Second session: verify data persisted
    {
        auto store = open_store();
        EXPECT_EQ(store->count(), 2);

        auto s1 = store->get(id1);
        ASSERT_TRUE(s1.ok());
        EXPECT_EQ(s1.value().name, "snippet1");
        EXPECT_EQ(s1.value().content, "content1");

        auto s2 = store->get(id2);
        ASSERT_TRUE(s2.ok());
        EXPECT_EQ(s2.value().name, "snippet2");
    }
}

TEST_F(SnippetStoreTest, TagsPersistAcrossReopen) {
    SnippetId id;

    {
        auto store = open_store();
        auto result = store->add("content", "snippet", {"initial"});
        ASSERT_TRUE(result.ok());
        id = result.value();
        store->add_tag(id, "added");
        store->close();
    }

    {
        auto store = open_store();
        auto snippet = store->get(id);
        ASSERT_TRUE(snippet.ok());
        EXPECT_EQ(snippet.value().tags.size(), 2);

        auto tags = store->get_all_tags();
        ASSERT_TRUE(tags.ok());
        EXPECT_EQ(tags.value().size(), 2);
    }
}

TEST_F(SnippetStoreTest, IdContinuesAcrossReopen) {
    SnippetId first_id, second_id;

    {
        auto store = open_store();
        auto result = store->add("content1", "snippet1", {});
        ASSERT_TRUE(result.ok());
        first_id = result.value();
        store->close();
    }

    {
        auto store = open_store();
        auto result = store->add("content2", "snippet2", {});
        ASSERT_TRUE(result.ok());
        second_id = result.value();
        EXPECT_GT(second_id, first_id);
    }
}

// ============================================================================
// Error Cases
// ============================================================================

TEST_F(SnippetStoreTest, DuplicateNameFails) {
    auto store = open_store();

    auto r1 = store->add("content1", "same-name", {});
    ASSERT_TRUE(r1.ok());

    auto r2 = store->add("content2", "same-name", {});
    EXPECT_FALSE(r2.ok());
    EXPECT_EQ(r2.error_code(), ErrorCode::ALREADY_EXISTS);
}

TEST_F(SnippetStoreTest, EmptyNameFails) {
    auto store = open_store();

    auto result = store->add("content", "", {});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error_code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(SnippetStoreTest, RemoveNonexistentFails) {
    auto store = open_store();

    auto result = store->remove(999);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error_code(), ErrorCode::NOT_FOUND);
}

TEST_F(SnippetStoreTest, AddTagToNonexistentFails) {
    auto store = open_store();

    auto result = store->add_tag(999, "tag");
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error_code(), ErrorCode::NOT_FOUND);
}

// ============================================================================
// Language Detector Unit Tests
// ============================================================================

TEST(LanguageDetectorTest, DetectFromExtension) {
    EXPECT_EQ(LanguageDetector::detect("", "script.py"), "python");
    EXPECT_EQ(LanguageDetector::detect("", "script.js"), "javascript");
    EXPECT_EQ(LanguageDetector::detect("", "script.ts"), "typescript");
    EXPECT_EQ(LanguageDetector::detect("", "script.cpp"), "cpp");
    EXPECT_EQ(LanguageDetector::detect("", "script.go"), "go");
    EXPECT_EQ(LanguageDetector::detect("", "script.rs"), "rust");
    EXPECT_EQ(LanguageDetector::detect("", "config.yaml"), "yaml");
    EXPECT_EQ(LanguageDetector::detect("", "config.json"), "json");
    EXPECT_EQ(LanguageDetector::detect("", "query.sql"), "sql");
}

TEST(LanguageDetectorTest, DetectFromShebang) {
    EXPECT_EQ(LanguageDetector::detect("#!/bin/bash\necho hi", ""), "bash");
    EXPECT_EQ(LanguageDetector::detect("#!/usr/bin/env python3\nprint(1)", ""), "python");
    EXPECT_EQ(LanguageDetector::detect("#!/usr/bin/env node\nconsole.log(1)", ""), "javascript");
    EXPECT_EQ(LanguageDetector::detect("#!/usr/bin/ruby\nputs 1", ""), "ruby");
}

TEST(LanguageDetectorTest, ShebangTakesPrecedence) {
    // Even with .py extension, bash shebang should win
    EXPECT_EQ(LanguageDetector::detect("#!/bin/bash\necho hi", "script.py"), "bash");
}

TEST(LanguageDetectorTest, SpecialFilenames) {
    EXPECT_EQ(LanguageDetector::detect("", "Dockerfile"), "dockerfile");
    EXPECT_EQ(LanguageDetector::detect("", "Makefile"), "makefile");
    EXPECT_EQ(LanguageDetector::detect("", ".bashrc"), "bash");
    EXPECT_EQ(LanguageDetector::detect("", ".zshrc"), "bash");
}

TEST(LanguageDetectorTest, UnknownReturnsText) {
    EXPECT_EQ(LanguageDetector::detect("random content", ""), "text");
    EXPECT_EQ(LanguageDetector::detect("", "file.unknown"), "text");
}
