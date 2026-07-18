#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/exception/io.h"
#include "common/file_system/virtual_file_system.h"
#include "gtest/gtest.h"
#include "test_helper/test_helper.h"

using namespace lbug::common;

namespace {

class RecordingFileSystem final : public FileSystem {
public:
    explicit RecordingFileSystem(std::string handledPrefix = "")
        : handledPrefix{std::move(handledPrefix)} {}

    bool canHandleFile(const std::string_view path) const override {
        return path.starts_with(handledPrefix);
    }

    std::unique_ptr<FileInfo> openFile(const std::string& path, FileOpenFlags,
        lbug::main::ClientContext* = nullptr) override {
        openedPaths.push_back(path);
        return std::make_unique<FileInfo>(path, this);
    }

    std::vector<std::string> glob(lbug::main::ClientContext*, const std::string&) const override {
        return {};
    }

    void overwriteFile(const std::string& from, const std::string& to) override {
        overwrites.emplace_back(from, to);
    }

    void renameFile(const std::string& from, const std::string& to) override {
        renames.emplace_back(from, to);
    }

    void copyFile(const std::string& from, const std::string& to) override {
        copies.emplace_back(from, to);
    }

    void removeFileIfExists(const std::string& path,
        const lbug::main::ClientContext* = nullptr) override {
        removedPaths.push_back(path);
    }

    void syncDirectory(const std::string& path) const override {
        syncedDirectories.push_back(path);
    }

    void syncFile(const FileInfo&) const override {}

    const std::string& databasePath() const { return dbPath; }

    std::vector<std::string> openedPaths;
    std::vector<std::pair<std::string, std::string>> overwrites;
    std::vector<std::pair<std::string, std::string>> renames;
    std::vector<std::pair<std::string, std::string>> copies;
    std::vector<std::string> removedPaths;
    mutable std::vector<std::string> syncedDirectories;

protected:
    void readFromFile(FileInfo&, void*, uint64_t, uint64_t) const override { UNREACHABLE_CODE; }
    int64_t readFile(FileInfo&, void*, size_t) const override { UNREACHABLE_CODE; }
    int64_t seek(FileInfo&, uint64_t, int) const override { UNREACHABLE_CODE; }
    uint64_t getFileSize(const FileInfo&) const override { return 0; }

private:
    std::string handledPrefix;
};

} // namespace

TEST(VFSTests, InjectedPrimaryHandlesDefaultPathsFromConstruction) {
    auto primary = std::make_unique<RecordingFileSystem>();
    auto* primaryPtr = primary.get();

    VirtualFileSystem vfs{"/tmp/injected.db", std::move(primary)};
    auto file = vfs.openFile("/tmp/injected.db", FileOpenFlags{FileFlags::READ_ONLY});

    ASSERT_NE(file, nullptr);
    EXPECT_EQ(primaryPtr->openedPaths, std::vector<std::string>{"/tmp/injected.db"});
    EXPECT_EQ(primaryPtr->databasePath(), "/tmp/injected.db");
}

TEST(VFSTests, InjectedPrimaryRejectsNullFileSystem) {
    EXPECT_ANY_THROW(VirtualFileSystem("/tmp/injected.db", nullptr));
}

TEST(VFSTests, TwoPathOperationsDispatchOnceWhenBothPathsUseSameBackend) {
    auto primary = std::make_unique<RecordingFileSystem>();
    auto secondary = std::make_unique<RecordingFileSystem>("secondary://");
    auto* secondaryPtr = secondary.get();
    VirtualFileSystem vfs{"/tmp/injected.db", std::move(primary)};
    vfs.registerFileSystem(std::move(secondary));

    vfs.renameFile("secondary://from", "secondary://to");
    vfs.overwriteFile("secondary://from", "secondary://to");
    vfs.copyFile("secondary://from", "secondary://to");

    const auto expectedOperation =
        std::vector<std::pair<std::string, std::string>>{{"secondary://from", "secondary://to"}};
    EXPECT_EQ(secondaryPtr->renames, expectedOperation);
    EXPECT_EQ(secondaryPtr->overwrites, expectedOperation);
    EXPECT_EQ(secondaryPtr->copies, expectedOperation);
}

TEST(VFSTests, TwoPathOperationsRejectDifferentBackendsWithoutDispatch) {
    auto primary = std::make_unique<RecordingFileSystem>();
    auto* primaryPtr = primary.get();
    auto secondary = std::make_unique<RecordingFileSystem>("secondary://");
    auto* secondaryPtr = secondary.get();
    VirtualFileSystem vfs{"/tmp/injected.db", std::move(primary)};
    vfs.registerFileSystem(std::move(secondary));

    EXPECT_ANY_THROW(vfs.renameFile("/tmp/from", "secondary://to"));
    EXPECT_ANY_THROW(vfs.overwriteFile("/tmp/from", "secondary://to"));
    EXPECT_ANY_THROW(vfs.copyFile("/tmp/from", "secondary://to"));

    EXPECT_TRUE(primaryPtr->renames.empty());
    EXPECT_TRUE(primaryPtr->overwrites.empty());
    EXPECT_TRUE(primaryPtr->copies.empty());
    EXPECT_TRUE(secondaryPtr->renames.empty());
    EXPECT_TRUE(secondaryPtr->overwrites.empty());
    EXPECT_TRUE(secondaryPtr->copies.empty());
}

TEST(VFSTests, SyncDirectoryRoutesToSelectedBackend) {
    auto primary = std::make_unique<RecordingFileSystem>();
    auto secondary = std::make_unique<RecordingFileSystem>("secondary://");
    auto* secondaryPtr = secondary.get();
    VirtualFileSystem vfs{"/tmp/injected.db", std::move(primary)};
    vfs.registerFileSystem(std::move(secondary));

    vfs.syncDirectory("secondary://parent");

    EXPECT_EQ(secondaryPtr->syncedDirectories, std::vector<std::string>{"secondary://parent"});
}

TEST(VFSTests, LegacyConstructorStillRoutesRegisteredSecondaryScheme) {
    VirtualFileSystem vfs{"/tmp/legacy.db"};
    auto secondary = std::make_unique<RecordingFileSystem>("secondary://");
    auto* secondaryPtr = secondary.get();
    vfs.registerFileSystem(std::move(secondary));

    auto file = vfs.openFile("secondary://file", FileOpenFlags{FileFlags::READ_ONLY});

    ASSERT_NE(file, nullptr);
    EXPECT_EQ(secondaryPtr->openedPaths, std::vector<std::string>{"secondary://file"});
}

TEST(VFSTests, RegisteredSecondaryCannotClaimBoundDatabaseSidecar) {
    auto primary = std::make_unique<RecordingFileSystem>();
    VirtualFileSystem vfs{"/tmp/injected.db", std::move(primary)};
    auto overlappingSecondary =
        std::make_unique<RecordingFileSystem>("/tmp/injected.db.wal");

    EXPECT_ANY_THROW(vfs.registerFileSystem(std::move(overlappingSecondary)));
}

TEST(VFSTests, InjectedPrimaryRejectsUnrelatedRemovalWithoutBackendDispatch) {
    const auto temporaryRoot = std::filesystem::path{
        lbug::testing::TestHelper::getTempDBPathStr("vfs_security_removal")}
                                   .parent_path();
    const auto unrelatedFile = temporaryRoot / "unrelated-file";
    const auto unrelatedDirectory = temporaryRoot / "unrelated-directory";
    std::ofstream{unrelatedFile}.put('x');
    std::filesystem::create_directory(unrelatedDirectory);
    auto primary = std::make_unique<RecordingFileSystem>();
    auto* primaryPtr = primary.get();
    VirtualFileSystem vfs{"/tmp/injected.db", std::move(primary)};

    EXPECT_THROW(vfs.removeFileIfExists(unrelatedFile.string()), IOException);
    EXPECT_THROW(vfs.removeFileIfExists(unrelatedDirectory.string()), IOException);

    EXPECT_TRUE(primaryPtr->removedPaths.empty());
    EXPECT_TRUE(std::filesystem::exists(unrelatedFile));
    EXPECT_TRUE(std::filesystem::exists(unrelatedDirectory));
    std::filesystem::remove_all(temporaryRoot);
}

TEST(VFSTests, RegisteredSecondaryCanRemoveItsOwnDisjointPath) {
    auto primary = std::make_unique<RecordingFileSystem>();
    auto secondary = std::make_unique<RecordingFileSystem>("secondary://");
    auto* secondaryPtr = secondary.get();
    VirtualFileSystem vfs{"/tmp/injected.db", std::move(primary)};
    vfs.registerFileSystem(std::move(secondary));

    EXPECT_NO_THROW(vfs.removeFileIfExists("secondary://owned-file"));

    EXPECT_EQ(secondaryPtr->removedPaths, std::vector<std::string>{"secondary://owned-file"});
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
