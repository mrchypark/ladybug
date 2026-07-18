#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/exception/io.h"
#include "common/exception/runtime.h"
#include "common/file_system/virtual_file_system.h"
#include "gtest/gtest.h"
#include "storage/storage_utils.h"

using namespace lbug::common;

class RoutingFileSystem final : public FileSystem {
public:
    explicit RoutingFileSystem(bool claimAll) : claimAll{claimAll} {}

    bool canHandleFile(const std::string_view /*path*/) const override { return claimAll; }

    bool fileOrPathExists(const std::string& path,
        lbug::main::ClientContext* /*context*/ = nullptr) override {
        observedPaths.push_back(path);
        return true;
    }

    void syncFile(const FileInfo& /*fileInfo*/) const override {}

    std::vector<std::string> observedPaths;

protected:
    void readFromFile(FileInfo& /*fileInfo*/, void* /*buffer*/, uint64_t /*numBytes*/,
        uint64_t /*position*/) const override {
        UNREACHABLE_CODE;
    }
    int64_t readFile(FileInfo& /*fileInfo*/, void* /*buf*/, size_t /*numBytes*/) const override {
        UNREACHABLE_CODE;
    }
    int64_t seek(FileInfo& /*fileInfo*/, uint64_t /*offset*/, int /*whence*/) const override {
        UNREACHABLE_CODE;
    }
    uint64_t getFileSize(const FileInfo& /*fileInfo*/) const override { UNREACHABLE_CODE; }

private:
    bool claimAll;
};

TEST(VFSTests, InjectedPrimaryRejectsNullFileSystem) {
    EXPECT_THROW(VirtualFileSystem("/tmp/null-primary.db", std::unique_ptr<FileSystem>{}),
        RuntimeException);
}

TEST(VFSTests, InjectedPrimaryDatabaseNamespacePrecedesRegisteredSubsystems) {
    const std::string databasePath = "/tmp/injected-primary.db";
    const auto graphPath = lbug::storage::StorageUtils::getGraphPath(databasePath, "analytics");
    const std::string otherParentGraphPath = "/var/tmp/injected-primary.analytics.db";
    const std::string nearPrefixPath = "/tmp/injected-primary.analytics.dbx";
    auto primary = std::make_unique<RoutingFileSystem>(false);
    auto* primaryPtr = primary.get();
    VirtualFileSystem vfs{databasePath, std::move(primary)};
    auto claimAll = std::make_unique<RoutingFileSystem>(true);
    auto* claimAllPtr = claimAll.get();
    vfs.registerFileSystem(std::move(claimAll));

    EXPECT_TRUE(vfs.fileOrPathExists(databasePath));
    EXPECT_TRUE(vfs.fileOrPathExists(databasePath + ".wal"));
    EXPECT_TRUE(vfs.fileOrPathExists(graphPath));
    EXPECT_TRUE(vfs.fileOrPathExists(graphPath + ".wal"));
    EXPECT_TRUE(vfs.fileOrPathExists(otherParentGraphPath));
    EXPECT_TRUE(vfs.fileOrPathExists(nearPrefixPath));
    EXPECT_TRUE(vfs.fileOrPathExists("/tmp/import.csv"));

    EXPECT_EQ(primaryPtr->observedPaths,
        (std::vector<std::string>{databasePath, databasePath + ".wal", graphPath,
            graphPath + ".wal"}));
    EXPECT_EQ(claimAllPtr->observedPaths,
        (std::vector<std::string>{otherParentGraphPath, nearPrefixPath, "/tmp/import.csv"}));
}

TEST(VFSTests, InjectedPrimaryInMemoryNamespacePrecedesRegisteredSubsystems) {
    auto primary = std::make_unique<RoutingFileSystem>(false);
    auto* primaryPtr = primary.get();
    VirtualFileSystem vfs{":memory:", std::move(primary)};
    auto claimAll = std::make_unique<RoutingFileSystem>(true);
    auto* claimAllPtr = claimAll.get();
    vfs.registerFileSystem(std::move(claimAll));

    EXPECT_TRUE(vfs.fileOrPathExists(":analytics"));
    EXPECT_TRUE(vfs.fileOrPathExists(":analytics.wal"));
    EXPECT_TRUE(vfs.fileOrPathExists("/tmp/import.csv"));

    EXPECT_EQ(primaryPtr->observedPaths,
        (std::vector<std::string>{":analytics", ":analytics.wal"}));
    EXPECT_EQ(claimAllPtr->observedPaths, (std::vector<std::string>{"/tmp/import.csv"}));
}

TEST(VFSTests, VirtualFileSystemDeleteFiles) {
    std::string homeDir = "/tmp/dbHome";
    lbug::common::VirtualFileSystem vfs(homeDir);
    std::filesystem::create_directories("/tmp/test1");

    // Attempt to delete files not within the list of db files (should error)
    try {
        vfs.removeFileIfExists("/tmp/test1");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp/test1 is not within the allowed "
                               "list of files to be removed.");
    }
    try {
        vfs.removeFileIfExists("/tmp/dbHome");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp/dbHome is not within the allowed "
                               "list of files to be removed.");
    }

    ASSERT_NO_THROW(vfs.removeFileIfExists("/tmp/dbHome.lock"));
    ASSERT_NO_THROW(vfs.removeFileIfExists("/tmp/dbHome.shadow"));
    ASSERT_NO_THROW(vfs.removeFileIfExists("/tmp/dbHome.wal"));
    ASSERT_NO_THROW(vfs.removeFileIfExists("/tmp/dbHome.tmp"));

    ASSERT_TRUE(std::filesystem::exists("/tmp/test1"));

    // Cleanup: Remove directories after the test
    std::filesystem::remove_all("/tmp/test1");
}

#ifndef __WASM__ // home directory is not available in WASM
TEST(VFSTests, VirtualFileSystemDeleteFilesWithHome) {
    std::string homeDir = "~/tmp/dbHome";
    lbug::common::VirtualFileSystem vfs(homeDir);
    std::filesystem::create_directories("~/tmp/test1");

    // Attempt to delete files outside the home directory (should error)
    try {
        vfs.removeFileIfExists("~/tmp/test1");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(), "IO exception: Error: Path ~/tmp/test1 is not within the allowed "
                               "list of files to be removed.");
    }
    try {
        vfs.removeFileIfExists("~/tmp/dbHome");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(), "IO exception: Error: Path ~/tmp/dbHome is not within the allowed "
                               "list of files to be removed.");
    }

    // Attempt to delete files outside the home directory (should error)
    try {
        vfs.removeFileIfExists("~");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(),
            "IO exception: Error: Path ~ is not within the allowed list of files to be removed.");
    }

    ASSERT_NO_THROW(vfs.removeFileIfExists("~/tmp/dbHome.lock"));
    ASSERT_NO_THROW(vfs.removeFileIfExists("~/tmp/dbHome.wal"));
    ASSERT_NO_THROW(vfs.removeFileIfExists("~/tmp/dbHome.shadow"));
    ASSERT_NO_THROW(vfs.removeFileIfExists("~/tmp/dbHome.tmp"));

    ASSERT_TRUE(std::filesystem::exists("~/tmp/test1"));

    // Cleanup: Remove directories after the test
    std::filesystem::remove_all("~/tmp/test1");
}
#endif

TEST(VFSTests, VirtualFileSystemDeleteFilesEdgeCases) {
    std::string homeDir = "/tmp/dbHome";
    lbug::common::VirtualFileSystem vfs(homeDir);
    std::filesystem::create_directories("/tmp/dbHome/");
    std::filesystem::create_directories("/tmp/dbHome/../test2");
    std::filesystem::create_directories("/tmp");
    std::filesystem::create_directories("/tmp/dbHome/test2");

    // Attempt to delete files outside the home directory (should error)
    try {
        vfs.removeFileIfExists("/tmp/dbHome/../test2");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp/dbHome/../test2 is not within the "
                               "allowed list of files to be removed.");
    }

    try {
        vfs.removeFileIfExists("/tmp");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp is not within the allowed list of "
                               "files to be removed.");
    }

    try {
        vfs.removeFileIfExists("/tmp/");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp/ is not within the allowed list of "
                               "files to be removed.");
    }

    try {
        vfs.removeFileIfExists("/tmp//////////////////");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp////////////////// is not within the "
                               "allowed list of files to be removed.");
    }

    try {
        vfs.removeFileIfExists("/tmp/./.././");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp/./.././ is not within the allowed "
                               "list of files to be removed.");
    }

    try {
        vfs.removeFileIfExists("/");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(),
            "IO exception: Error: Path / is not within the allowed list of files to be removed.");
    }

    try {
        vfs.removeFileIfExists("/tmp/dbHome/test2");
    } catch (const lbug::common::IOException& e) {
        // Expected behavior
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp/dbHome/test2 is not within the "
                               "allowed list of files to be removed.");
    }

    ASSERT_TRUE(std::filesystem::exists("/tmp/test2"));
    ASSERT_TRUE(std::filesystem::exists("/tmp/dbHome/test2"));

    // Cleanup: Remove directories after the test
    std::filesystem::remove_all("/tmp/test2");
    std::filesystem::remove_all("/tmp/dbHome/test2");
}

#if defined(_WIN32)
TEST(VFSTests, VirtualFileSystemDeleteFilesWindowsPaths) {
    // Test Home Directory
    std::string homeDir = "C:\\Desktop\\dir";
    lbug::common::VirtualFileSystem vfs(homeDir);

    // Setup directories for testing
    std::filesystem::create_directories("C:\\test1");

    // Mixed separators: HomeDir uses '\' while path uses '/'
    std::string mixedSeparatorPath = "C:\\Desktop/dir/test1";

    // Attempt to delete files outside the home directory (should error)
    try {
        vfs.removeFileIfExists("C:\\test1");
        FAIL() << "Expected exception for path outside home directory.";
    } catch (const lbug::common::IOException& e) {
        EXPECT_STREQ(e.what(), "IO exception: Error: Path C:\\test1 is not within the allowed list "
                               "of files to be removed.");
    }

    // Attempt to delete file inside the home directory with mixed separators (should succeed)
    try {
        vfs.removeFileIfExists(mixedSeparatorPath);
    } catch (const lbug::common::IOException& e) {
        EXPECT_STREQ(e.what(), "IO exception: Error: Path C:\\Desktop/dir/test1 is not within the "
                               "allowed list of files to be removed.");
    }

    ASSERT_FALSE(std::filesystem::exists("C:\\Desktop\\dir\\test1")); // Should be deleted

    // Cleanup
    std::filesystem::remove_all("C:\\Desktop\\dir");
}
#endif

TEST(VFSTests, VirtualFileSystemDeleteFilesWildcardNoRemoval) {
    // Test Home Directory
    std::string homeDir = "/tmp/dbHome_wildcard/";
    lbug::common::VirtualFileSystem vfs(homeDir);

    // Setup files and directories
    std::filesystem::create_directories("/tmp/dbHome_wildcard/test1_wildcard");
    std::filesystem::create_directories("/tmp/dbHome_wildcard/test2_wildcard");
    std::filesystem::create_directories("/tmp/dbHome_wildcard/nested_wildcard");
    std::ofstream("/tmp/dbHome_wildcard/nested_wildcard/file1.txt").close();
    std::ofstream("/tmp/dbHome_wildcard/nested_wildcard/file2.test").close();

    // Attempt to remove files with wildcard pattern
    try {
        vfs.removeFileIfExists("/tmp/dbHome_wildcard/test*");
    } catch (const lbug::common::IOException& e) {
        // Verify the exception is thrown for unsupported wildcard
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp/dbHome_wildcard/test* is not within "
                               "the allowed list of files to be removed.");
    }

    // Verify files and directories still exist
    ASSERT_TRUE(std::filesystem::exists("/tmp/dbHome_wildcard/test1_wildcard"));
    ASSERT_TRUE(std::filesystem::exists("/tmp/dbHome_wildcard/test2_wildcard"));
    ASSERT_TRUE(std::filesystem::exists("/tmp/dbHome_wildcard/nested_wildcard/file1.txt"));
    ASSERT_TRUE(std::filesystem::exists("/tmp/dbHome_wildcard/nested_wildcard/file2.test"));

    // Cleanup
    std::filesystem::remove_all("/tmp/dbHome_wildcard");
}

TEST(VFSTests, VirtualFileSystemDeleteFilesPatternValidation) {
    std::string dbPath = "/tmp/foo.db";
    lbug::common::VirtualFileSystem vfs(dbPath);

    std::filesystem::create_directories("/tmp/foo.db");

    ASSERT_NO_THROW(vfs.removeFileIfExists("/tmp/foo.db.wal"));
    ASSERT_NO_THROW(vfs.removeFileIfExists("/tmp/foo.db.shadow"));
    ASSERT_NO_THROW(vfs.removeFileIfExists("/tmp/foo.db.tmp"));

    ASSERT_NO_THROW(vfs.removeFileIfExists("/tmp/foo.db.graph1.wal"));
    ASSERT_NO_THROW(vfs.removeFileIfExists("/tmp/foo.db.graph1.shadow"));
    ASSERT_NO_THROW(vfs.removeFileIfExists("/tmp/foo.db.graph1.tmp"));
    ASSERT_NO_THROW(vfs.removeFileIfExists("/tmp/foo.db.graph1.db"));

    try {
        vfs.removeFileIfExists("/tmp/foo.db");
        FAIL() << "Expected exception for foo.db (dbPath itself)";
    } catch (const lbug::common::IOException& e) {
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp/foo.db is not within the allowed "
                               "list of files to be removed.");
    }

    try {
        vfs.removeFileIfExists("/tmp/bar.db");
        FAIL() << "Expected exception for bar.db when dbPath is foo.db";
    } catch (const lbug::common::IOException& e) {
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp/bar.db is not within the allowed "
                               "list of files to be removed.");
    }

    try {
        vfs.removeFileIfExists("/tmp/foo.wal");
        FAIL() << "Expected exception for foo.wal (stem doesn't match foo.db)";
    } catch (const lbug::common::IOException& e) {
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp/foo.wal is not within the allowed "
                               "list of files to be removed.");
    }

    try {
        vfs.removeFileIfExists("/tmp/foo.db.arbitrary");
        FAIL() << "Expected exception for foo.db.arbitrary (arbitrary extension)";
    } catch (const lbug::common::IOException& e) {
        EXPECT_STREQ(e.what(), "IO exception: Error: Path /tmp/foo.db.arbitrary is not within the "
                               "allowed list of files to be removed.");
    }

    ASSERT_TRUE(std::filesystem::exists("/tmp/foo.db"));

    std::filesystem::remove_all("/tmp/foo.db");
}
