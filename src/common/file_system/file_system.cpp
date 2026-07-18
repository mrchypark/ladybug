#include "common/file_system/file_system.h"

#include "common/exception/io.h"
#include "common/string_utils.h"
#include "main/client_context.h"
#include <format>

namespace lbug {
namespace common {

void FileSystem::overwriteFile(const std::string& /*from*/, const std::string& /*to*/) {
    throw IOException("Overwrite is unsupported by this filesystem.");
}

void FileSystem::renameFile(const std::string& /*from*/, const std::string& /*to*/) {
    throw IOException("Rename is unsupported by this filesystem.");
}

void FileSystem::copyFile(const std::string& /*from*/, const std::string& /*to*/) {
    throw IOException("Copy is unsupported by this filesystem.");
}

void FileSystem::createDir(const std::string& /*dir*/) const {
    UNREACHABLE_CODE;
}

void FileSystem::removeFileIfExists(const std::string&, const main::ClientContext* /*context*/) {
    UNREACHABLE_CODE;
}

bool FileSystem::fileOrPathExists(const std::string& /*path*/, main::ClientContext* /*context*/) {
    UNREACHABLE_CODE;
}

bool FileSystem::isDirectory(const std::string& /*path*/) const {
    throw IOException("Determining whether a path is a directory is unsupported by this "
                      "filesystem.");
}

std::string FileSystem::expandPath(main::ClientContext* /*context*/,
    const std::string& path) const {
    return path;
}

void FileSystem::syncDirectory(const std::string& /*directoryPath*/) const {
    throw IOException("Directory sync is unsupported by this filesystem.");
}

void FileSystem::bindDatabasePath(std::string path) {
    if (databasePathBound) {
        throw IOException("The database path is already bound to this filesystem.");
    }
    dbPath = std::move(path);
    databasePathBound = true;
}

bool FileSystem::isAllowedRemovalPath(const std::string& path, const std::string& databasePath,
    const main::ClientContext* context) {
    const auto pathObject = std::filesystem::path(path);
    const auto databasePathObject = std::filesystem::path(databasePath);
    const auto databaseDirectory = databasePathObject.parent_path();
    if (pathObject.is_absolute() && databasePathObject.is_absolute() &&
        pathObject.parent_path() != databaseDirectory) {
        return false;
    }

    const auto fileName = pathObject.filename().string();
    const auto extension = pathObject.extension().string();
    const auto stemWithoutExtension = pathObject.stem().string();
    const auto databaseBase = databasePathObject.stem().string();
    const auto databaseExtension = databasePathObject.extension().string();
    const auto databaseFileName = databaseBase + databaseExtension;

    if (extension == ".wal" || extension == ".shadow" || extension == ".tmp" ||
        extension == ".lock" || extension == ".checkpoint") {
        if (stemWithoutExtension == databaseFileName) {
            return true;
        }
        if ((stemWithoutExtension.starts_with(databaseBase + ".") &&
                stemWithoutExtension.ends_with(databaseExtension)) ||
            stemWithoutExtension.starts_with(databaseFileName + ".")) {
            return true;
        }
    } else if (extension == databaseExtension && fileName.starts_with(databaseBase + ".") &&
               fileName != databaseFileName) {
        return true;
    }

    if (context == nullptr) {
        return false;
    }
    const auto relativePath = std::filesystem::relative(path, context->getExtensionDir());
    for (const auto& part : relativePath) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::string FileSystem::joinPath(const std::string& base, const std::string& part) {
    return base + "/" + part;
}

std::string FileSystem::getFileExtension(const std::filesystem::path& path) {
    auto extension = path.extension();
    if (isCompressedFile(path)) {
        extension = path.stem().extension();
    }
    return extension.string();
}

bool FileSystem::isCompressedFile(const std::filesystem::path& path) {
    return isGZIPCompressed(path);
}

std::string FileSystem::getFileName(const std::filesystem::path& path) {
    return path.filename().string();
}

void FileSystem::writeFile(FileInfo& /*fileInfo*/, const uint8_t* /*buffer*/, uint64_t /*numBytes*/,
    uint64_t /*offset*/) const {
    UNREACHABLE_CODE;
}

void FileSystem::truncate(FileInfo& /*fileInfo*/, uint64_t /*size*/) const {
    UNREACHABLE_CODE;
}

void FileSystem::reset(FileInfo& fileInfo) {
    fileInfo.seek(0, SEEK_SET);
}

bool FileSystem::isGZIPCompressed(const std::filesystem::path& path) {
    auto extensionLowerCase = StringUtils::getLower(path.extension().string());
    return extensionLowerCase == ".gz" || extensionLowerCase == ".gzip";
}

} // namespace common
} // namespace lbug
