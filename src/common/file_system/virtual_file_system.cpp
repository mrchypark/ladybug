#include "common/file_system/virtual_file_system.h"

#include <cctype>

#include "common/assert.h"
#include "common/exception/io.h"
#include "common/exception/runtime.h"
#include "common/file_system/gzip_file_system.h"
#include "common/file_system/local_file_system.h"
#include "common/string_utils.h"
#include "main/client_context.h"
#include "main/database.h"

namespace lbug {
namespace common {

static bool isRelativePath(const std::string& path) {
    if (path.empty() || path[0] == '/' || path[0] == '~') {
        return false;
    }
    if (path.find("://") != std::string::npos) {
        return false;
    }
    return !(
        path.size() > 1 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':');
}

static std::string joinRemotePath(const std::string& base, std::string path) {
    while (path.starts_with("./")) {
        path = path.substr(2);
    }
    if (path == ".") {
        path.clear();
    }
    if (path.empty() || base.ends_with('/')) {
        return base + path;
    }
    return base + "/" + path;
}

VirtualFileSystem::VirtualFileSystem() : VirtualFileSystem{""} {}

VirtualFileSystem::VirtualFileSystem(std::string homeDir) {
    defaultFS = std::make_unique<LocalFileSystem>(std::move(homeDir));
    compressedFileSystem.emplace(FileCompressionType::GZIP, std::make_unique<GZipFileSystem>());
}

VirtualFileSystem::VirtualFileSystem(std::string databasePath,
    std::unique_ptr<FileSystem> primaryFileSystem)
    : FileSystem{std::move(databasePath)}, defaultFS{std::move(primaryFileSystem)} {
    if (!defaultFS) {
        throw RuntimeException{"The primary filesystem cannot be null."};
    }
    compressedFileSystem.emplace(FileCompressionType::GZIP, std::make_unique<GZipFileSystem>());
}

VirtualFileSystem::~VirtualFileSystem() = default;

void VirtualFileSystem::registerFileSystem(std::unique_ptr<FileSystem> fileSystem) {
    subSystems.push_back(std::move(fileSystem));
}

FileCompressionType VirtualFileSystem::autoDetectCompressionType(const std::string& path) {
    if (isGZIPCompressed(path)) {
        return FileCompressionType::GZIP;
    }
    return FileCompressionType::UNCOMPRESSED;
}

std::unique_ptr<FileInfo> VirtualFileSystem::openFile(const std::string& path, FileOpenFlags flags,
    main::ClientContext* context) {
    auto compressionType = flags.compressionType;
    if (compressionType == FileCompressionType::AUTO_DETECT) {
        compressionType = autoDetectCompressionType(path);
    }
    auto fileHandle = findFileSystem(path)->openFile(path, flags, context);
    if (compressionType == FileCompressionType::UNCOMPRESSED) {
        return fileHandle;
    }
    if (flags.flags & FileFlags::WRITE) {
        throw IOException{"Writing to compressed files is not supported yet."};
    }
    if (StringUtils::getLower(getFileExtension(path)) != ".csv") {
        throw IOException{"Lbug currently only supports reading from compressed csv files."};
    }
    return compressedFileSystem.at(compressionType)->openCompressedFile(std::move(fileHandle));
}

std::vector<std::string> VirtualFileSystem::glob(main::ClientContext* context,
    const std::string& path) const {
    return findFileSystem(path)->glob(context, path);
}

void VirtualFileSystem::overwriteFile(const std::string& from, const std::string& to) {
    findFileSystem(from)->overwriteFile(from, to);
}

void VirtualFileSystem::renameFile(const std::string& from, const std::string& to) {
    defaultFS->renameFile(from, to);
}

void VirtualFileSystem::createDir(const std::string& dir) const {
    findFileSystem(dir)->createDir(dir);
}

void VirtualFileSystem::removeFileIfExists(const std::string& path,
    const main::ClientContext* context) {
    findFileSystem(path)->removeFileIfExists(path, context);
}

bool VirtualFileSystem::fileOrPathExists(const std::string& path, main::ClientContext* context) {
    return findFileSystem(path)->fileOrPathExists(path, context);
}

std::string VirtualFileSystem::expandPath(main::ClientContext* context,
    const std::string& path) const {
    return findFileSystem(path)->expandPath(context, path);
}

void VirtualFileSystem::readFromFile(FileInfo& /*fileInfo*/, void* /*buffer*/,
    uint64_t /*numBytes*/, uint64_t /*position*/) const {
    UNREACHABLE_CODE;
}

int64_t VirtualFileSystem::readFile(FileInfo& /*fileInfo*/, void* /*buf*/, size_t /*nbyte*/) const {
    UNREACHABLE_CODE;
}

void VirtualFileSystem::writeFile(FileInfo& /*fileInfo*/, const uint8_t* /*buffer*/,
    uint64_t /*numBytes*/, uint64_t /*offset*/) const {
    UNREACHABLE_CODE;
}

void VirtualFileSystem::syncFile(const FileInfo& fileInfo) const {
    findFileSystem(fileInfo.path)->syncFile(fileInfo);
}

void VirtualFileSystem::cleanUP(main::ClientContext* context) {
    for (auto& subSystem : subSystems) {
        subSystem->cleanUP(context);
    }
    defaultFS->cleanUP(context);
}

bool VirtualFileSystem::handleFileViaFunction(const std::string& path) const {
    return findFileSystem(path)->handleFileViaFunction(path);
}

function::TableFunction VirtualFileSystem::getHandleFunction(const std::string& path) const {
    return findFileSystem(path)->getHandleFunction(path);
}

int64_t VirtualFileSystem::seek(FileInfo& /*fileInfo*/, uint64_t /*offset*/, int /*whence*/) const {
    UNREACHABLE_CODE;
}

void VirtualFileSystem::truncate(FileInfo& /*fileInfo*/, uint64_t /*size*/) const {
    UNREACHABLE_CODE;
}

uint64_t VirtualFileSystem::getFileSize(const FileInfo& /*fileInfo*/) const {
    UNREACHABLE_CODE;
}

FileSystem* VirtualFileSystem::findFileSystem(const std::string& path) const {
    if (isInPrimaryDatabaseNamespace(path)) {
        return defaultFS.get();
    }
    for (auto& subSystem : subSystems) {
        if (subSystem->canHandleFile(path)) {
            return subSystem.get();
        }
    }
    return defaultFS.get();
}

bool VirtualFileSystem::isInPrimaryDatabaseNamespace(const std::string& path) const {
    if (dbPath.empty()) {
        return false;
    }
    const std::string_view databasePath{dbPath};
    const std::string_view candidatePath{path};
    if (candidatePath == databasePath ||
        (candidatePath.starts_with(databasePath) && candidatePath.size() > databasePath.size() &&
            candidatePath[databasePath.size()] == '.')) {
        return true;
    }
    if (databasePath == ":memory:" && candidatePath.starts_with(':')) {
        return true;
    }

    const auto databaseSeparator = databasePath.find_last_of("/\\");
    const auto candidateSeparator = candidatePath.find_last_of("/\\");
    const auto databaseParentSize =
        databaseSeparator == std::string_view::npos ? 0 : databaseSeparator + 1;
    const auto candidateParentSize =
        candidateSeparator == std::string_view::npos ? 0 : candidateSeparator + 1;
    if (candidateParentSize != databaseParentSize ||
        candidatePath.substr(0, candidateParentSize) !=
            databasePath.substr(0, databaseParentSize)) {
        return false;
    }
    const auto databaseName = databasePath.substr(databaseParentSize);
    const auto candidateName = candidatePath.substr(candidateParentSize);
    const auto extensionPosition = databaseName.find_last_of('.');
    if (extensionPosition == std::string_view::npos || extensionPosition == 0) {
        return false;
    }
    const auto stem = databaseName.substr(0, extensionPosition);
    const auto extension = databaseName.substr(extensionPosition);
    if (!candidateName.starts_with(stem) || candidateName.size() <= stem.size() + 1 ||
        candidateName[stem.size()] != '.') {
        return false;
    }
    const auto graphAndSuffix = candidateName.substr(stem.size() + 1);
    for (auto position = graphAndSuffix.find(extension); position != std::string::npos;
         position = graphAndSuffix.find(extension, position + 1)) {
        const auto suffixPosition = position + extension.size();
        if (position > 0 &&
            (suffixPosition == graphAndSuffix.size() || graphAndSuffix[suffixPosition] == '.')) {
            return true;
        }
    }
    return false;
}

VirtualFileSystem* VirtualFileSystem::GetUnsafe(const main::ClientContext& context) {
    return context.getDatabase()->getVFS();
}

std::string VirtualFileSystem::resolvePath(main::ClientContext* context, const std::string& path) {
    if (!context) {
        return path;
    }
    auto vfs = GetUnsafe(*context);
    if (!vfs) {
        return path;
    }
    auto paths = vfs->glob(context, path);
    if (!paths.empty()) {
        return paths.front();
    }
    if (!isRelativePath(path)) {
        return path;
    }
    const auto& fileSearchPath = context->getClientConfig()->fileSearchPath;
    if (fileSearchPath.empty()) {
        return path;
    }
    for (auto& searchPath : StringUtils::split(fileSearchPath, ",")) {
        if (searchPath.find("://") == std::string::npos) {
            continue;
        }
        paths = vfs->glob(context, joinRemotePath(searchPath, path));
        if (!paths.empty()) {
            return paths.front();
        }
    }
    return path;
}

} // namespace common
} // namespace lbug
