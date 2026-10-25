#include "nmarkdown/platform/platform.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>

namespace {

bool has_reader_extension(std::string path) {
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".tns") == 0) {
        path.resize(path.size() - 4);
    }
    const char* extensions[] = {".md", ".markdown", ".txt"};
    for (const char* extension : extensions) {
        const std::size_t length = std::strlen(extension);
        if (path.size() >= length &&
            path.compare(path.size() - length, length, extension) == 0) {
            return true;
        }
    }
    return false;
}

bool has_font_file_extension(std::string path) {
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".tns") == 0) {
        path.resize(path.size() - 4);
    }
    return path.size() >= 4 &&
           (path.compare(path.size() - 4, 4, ".ttf") == 0 ||
            path.compare(path.size() - 4, 4, ".otf") == 0);
}

using PathPredicate = bool (*)(std::string);

constexpr std::size_t kMaximumDocumentEntriesScanned = 8192;
constexpr std::size_t kMaximumFontEntriesScanned = 2048;

enum class DirectoryEntryKind {
    Directory,
    RegularFile,
    Other,
};

DirectoryEntryKind directory_entry_kind(const dirent& entry,
                                         const std::string& path) {
    (void)entry;
#if defined(DT_DIR) && defined(DT_REG) && defined(DT_UNKNOWN)
    if (entry.d_type == DT_DIR) return DirectoryEntryKind::Directory;
    if (entry.d_type == DT_REG) return DirectoryEntryKind::RegularFile;
    // Do not follow symbolic links. Besides avoiding directory cycles, this
    // keeps discovery work predictable on filesystems that report a type.
    if (entry.d_type != DT_UNKNOWN) return DirectoryEntryKind::Other;
#endif
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) return DirectoryEntryKind::Other;
    if (S_ISDIR(info.st_mode)) return DirectoryEntryKind::Directory;
    if (S_ISREG(info.st_mode)) return DirectoryEntryKind::RegularFile;
    return DirectoryEntryKind::Other;
}

bool replacement_rename_unavailable(int error) {
    if (error == ENOSYS) return true;
#if defined(ENOTSUP)
    if (error == ENOTSUP) return true;
#endif
#if defined(EOPNOTSUPP)
    if (error == EOPNOTSUPP) return true;
#endif
    return false;
}

int rename_for_replacement(const char* source, const char* destination) {
#if defined(NMARKDOWN_TEST_RENAME_ENOSYS)
    // The focused host test emulates both Ndless observations: an existing
    // destination makes the first rename report ENOSYS, while a new state file
    // first reports ENOENT and then gets ENOSYS for .tmp -> final.
    const std::string source_path(source == nullptr ? "" : source);
    struct stat info {};
    const bool source_exists = !source_path.empty() &&
                               stat(source_path.c_str(), &info) == 0;
    const bool temporary_source = source_path.size() >= 4 &&
        source_path.compare(source_path.size() - 4, 4, ".tmp") == 0;
    // Exercise the mixed behavior seen on-device: moving the existing final
    // file out of the way succeeds, but committing the completed temporary
    // file reports ENOSYS. The rollback variant also makes the direct rewrite
    // fail after that successful first rename.
    if (source_path.find("rename-mixed-") != std::string::npos) {
        if (temporary_source) {
            if (source_path.find("rename-mixed-rollback-") !=
                std::string::npos) {
                mkdir(destination, 0700);
            }
            errno = ENOSYS;
            return -1;
        }
        return std::rename(source, destination);
    }
    if (source_exists || temporary_source) {
        errno = ENOSYS;
        return -1;
    }
    return std::rename(source, destination);
#else
    return std::rename(source, destination);
#endif
}

bool write_complete_file(const std::string& path,
                         const std::uint8_t* data,
                         std::size_t size,
                         const char* description,
                         std::string& error) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        error = std::string("could not open ") + description + ": " +
                std::strerror(errno);
        return false;
    }
    const std::size_t written = size == 0 ? 0 : std::fwrite(data, 1, size, file);
    const bool write_failed = written != size || std::ferror(file) != 0 ||
                              std::fflush(file) != 0;
    const bool close_failed = std::fclose(file) != 0;
    if (write_failed || close_failed) {
        error = std::string("could not finish ") + description;
        return false;
    }
    return true;
}

bool file_matches(const std::string& path,
                  const std::uint8_t* expected,
                  std::size_t size,
                  std::string& error) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = std::string("could not verify replacement state file: ") +
                std::strerror(errno);
        return false;
    }

    std::uint8_t buffer[1024];
    std::size_t offset = 0;
    bool matches = true;
    while (offset < size) {
        const std::size_t requested = std::min(sizeof(buffer), size - offset);
        const std::size_t count = std::fread(buffer, 1, requested, file);
        if (count != requested ||
            std::memcmp(buffer, expected + offset, requested) != 0) {
            matches = false;
            break;
        }
        offset += count;
    }
    if (matches) {
        const int trailing = std::fgetc(file);
        matches = trailing == EOF && std::ferror(file) == 0;
    }
    const bool close_failed = std::fclose(file) != 0;
    if (!matches || close_failed) {
        error = "could not verify replacement state file";
        return false;
    }
    return true;
}

bool copy_file_verified(const std::string& source,
                        const std::string& destination,
                        const char* description,
                        std::string& error) {
    FILE* input = std::fopen(source.c_str(), "rb");
    if (input == nullptr) {
        error = std::string("could not open ") + description + ": " +
                std::strerror(errno);
        return false;
    }
    FILE* output = std::fopen(destination.c_str(), "wb");
    if (output == nullptr) {
        const int open_error = errno;
        std::fclose(input);
        error = std::string("could not create ") + description + ": " +
                std::strerror(open_error);
        return false;
    }

    std::uint8_t buffer[1024];
    bool copied = true;
    while (true) {
        const std::size_t count = std::fread(buffer, 1, sizeof(buffer), input);
        if (count != 0 && std::fwrite(buffer, 1, count, output) != count) {
            copied = false;
            break;
        }
        if (count != sizeof(buffer)) {
            if (std::ferror(input) != 0) copied = false;
            break;
        }
    }
    if (copied && std::fflush(output) != 0) copied = false;
    const bool input_close_failed = std::fclose(input) != 0;
    const bool output_close_failed = std::fclose(output) != 0;
    if (!copied || input_close_failed || output_close_failed) {
        std::remove(destination.c_str());
        error = std::string("could not finish ") + description;
        return false;
    }

    FILE* expected = std::fopen(source.c_str(), "rb");
    FILE* actual = std::fopen(destination.c_str(), "rb");
    if (expected == nullptr || actual == nullptr) {
        if (expected != nullptr) std::fclose(expected);
        if (actual != nullptr) std::fclose(actual);
        std::remove(destination.c_str());
        error = std::string("could not verify ") + description;
        return false;
    }
    bool matches = true;
    std::uint8_t expected_buffer[1024];
    std::uint8_t actual_buffer[1024];
    while (true) {
        const std::size_t expected_count =
            std::fread(expected_buffer, 1, sizeof(expected_buffer), expected);
        const std::size_t actual_count =
            std::fread(actual_buffer, 1, sizeof(actual_buffer), actual);
        if (expected_count != actual_count ||
            std::memcmp(expected_buffer, actual_buffer, expected_count) != 0) {
            matches = false;
            break;
        }
        if (expected_count != sizeof(expected_buffer)) {
            if (std::ferror(expected) != 0 || std::ferror(actual) != 0) {
                matches = false;
            }
            break;
        }
    }
    const bool expected_close_failed = std::fclose(expected) != 0;
    const bool actual_close_failed = std::fclose(actual) != 0;
    if (!matches || expected_close_failed || actual_close_failed) {
        std::remove(destination.c_str());
        error = std::string("could not verify ") + description;
        return false;
    }
    return true;
}

template <typename Progress>
bool collect_matching_files(const std::string& root,
                            unsigned depth,
                            std::size_t maximum_results,
                            std::vector<std::string>& paths,
                            std::string& error,
                            PathPredicate matches,
                            const char* folder_kind,
                            bool& truncated,
                            std::size_t& entries_remaining,
                            std::size_t& entries_since_progress,
                            Progress& progress) {
    if (truncated) return true;
    if (depth > 12) {
        truncated = true;
        return true;
    }
    DIR* directory = opendir(root.c_str());
    if (directory == nullptr) {
        if (depth == 0) {
            error = std::string("could not open ") + folder_kind + " folder: " +
                    std::strerror(errno);
            return false;
        }
        return true;
    }
    // Finish the current directory before descending. A font copied directly
    // to My Documents must not be hidden behind an earlier, very large
    // subdirectory in readdir() order.
    std::vector<std::string> directories;
    while (!truncated) {
        dirent* entry = readdir(directory);
        if (entry == nullptr) break;
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0 ||
            name[0] == '.') {
            continue;
        }
        if (entries_remaining == 0) {
            truncated = true;
            break;
        }
        --entries_remaining;
        if (++entries_since_progress >= 16) {
            entries_since_progress = 0;
            progress();
        }
        std::string path = root;
        if (!path.empty() && path.back() != '/') path.push_back('/');
        path += name;
        const DirectoryEntryKind kind = directory_entry_kind(*entry, path);
        if (kind == DirectoryEntryKind::Directory) {
            directories.push_back(std::move(path));
        } else if (kind == DirectoryEntryKind::RegularFile && matches(path)) {
            if (paths.size() < maximum_results) {
                paths.push_back(std::move(path));
            } else {
                truncated = true;
            }
        }
    }
    closedir(directory);
    for (const std::string& path : directories) {
        if (truncated) break;
        if (!collect_matching_files(path, depth + 1, maximum_results,
                                    paths, error, matches, folder_kind,
                                    truncated, entries_remaining,
                                    entries_since_progress, progress)) {
            return false;
        }
    }
    return true;
}

}  // namespace

namespace nmarkdown {

namespace {

class StdioRandomAccessData final : public RandomAccessData {
public:
    StdioRandomAccessData(FILE* file, std::uint64_t size)
        : file_(file), size_(size) {}

    ~StdioRandomAccessData() override {
        if (file_ != nullptr) std::fclose(file_);
    }

    std::uint64_t size() const override { return size_; }

    bool read(std::uint64_t offset,
              std::uint8_t* data,
              std::size_t size) override {
        if (file_ == nullptr || (data == nullptr && size != 0) ||
            offset > size_ || size > size_ - offset ||
            offset > static_cast<std::uint64_t>(LONG_MAX)) {
            return false;
        }
        if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
            return false;
        }
        if (size == 0) return true;
        const std::size_t count = std::fread(data, 1, size, file_);
        return count == size && std::ferror(file_) == 0;
    }

private:
    FILE* file_ = nullptr;
    std::uint64_t size_ = 0;
};

}  // namespace

bool StdioFileSystem::probe(const char* path,
                            DocumentProbe& result,
                            std::string& error) {
    result = {};
    error.clear();

    if (path == nullptr || path[0] == '\0') {
        error = "document path is empty";
        return false;
    }

    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        error = std::string("could not open document: ") + std::strerror(errno);
        return false;
    }

    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long end = std::ftell(file);
        if (end >= 0) {
            result.size = static_cast<std::uint64_t>(end);
        }
        std::fseek(file, 0, SEEK_SET);
    }

    std::uint8_t sample[4096];
    const std::size_t count = std::fread(sample, 1, sizeof(sample), file);
    if (std::ferror(file)) {
        error = std::string("could not read document: ") + std::strerror(errno);
        std::fclose(file);
        return false;
    }

    result.bytes_sampled = static_cast<std::uint32_t>(count);
    if (result.size == 0 && count > 0) {
        result.size = count;
    }
    result.sample_truncated = result.size > count;

    std::uint32_t hash = 2166136261U;
    for (std::size_t index = 0; index < count; ++index) {
        hash ^= sample[index];
        hash *= 16777619U;
    }
    result.sample_hash = hash;

    std::fclose(file);
    return true;
}

bool StdioFileSystem::read_all(const char* path,
                               std::size_t maximum_size,
                               std::vector<std::uint8_t>& data,
                               std::string& error) {
    data.clear();
    error.clear();
    if (path == nullptr || path[0] == '\0') {
        error = "document path is empty";
        return false;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        error = std::string("could not open document: ") + std::strerror(errno);
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        error = "could not seek document";
        return false;
    }
    const long end = std::ftell(file);
    if (end < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        error = "could not determine document size";
        return false;
    }
    const std::size_t size = static_cast<std::size_t>(end);
    if (size > maximum_size) {
        std::fclose(file);
        error = "file exceeds the configured size limit";
        return false;
    }
    data.resize(size);
    constexpr std::size_t kReadChunkBytes = 32U * 1024U;
    std::size_t read = 0;
    while (read < size) {
        const std::size_t requested =
            std::min(kReadChunkBytes, size - read);
        const std::size_t count =
            std::fread(data.data() + read, 1, requested, file);
        read += count;
        if (count != requested || std::ferror(file) != 0) break;
        if (read < size) report_operation_progress(read, size);
    }
    const bool read_failed = read != size || std::ferror(file) != 0;
    const bool close_failed = std::fclose(file) != 0;
    const bool failed = read_failed || close_failed;
    if (failed) {
        data.clear();
        error = "could not read complete document";
        return false;
    }
    return true;
}

bool StdioFileSystem::read_all_text(const char* path,
                                    std::size_t maximum_size,
                                    std::string& data,
                                    std::string& error) {
    data.clear();
    error.clear();
    if (path == nullptr || path[0] == '\0') {
        error = "document path is empty";
        return false;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        error = std::string("could not open document: ") + std::strerror(errno);
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        error = "could not seek document";
        return false;
    }
    const long end = std::ftell(file);
    if (end < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        error = "could not determine document size";
        return false;
    }
    const std::size_t size = static_cast<std::size_t>(end);
    if (size > maximum_size) {
        std::fclose(file);
        error = "file exceeds the configured size limit";
        return false;
    }
    data.resize(size);
    constexpr std::size_t kReadChunkBytes = 32U * 1024U;
    std::size_t read = 0;
    while (read < size) {
        const std::size_t requested =
            std::min(kReadChunkBytes, size - read);
        const std::size_t count =
            std::fread(&data[read], 1, requested, file);
        read += count;
        if (count != requested || std::ferror(file) != 0) break;
        if (read < size) report_operation_progress(read, size);
    }
    const bool read_failed = read != size || std::ferror(file) != 0;
    const bool close_failed = std::fclose(file) != 0;
    if (read_failed || close_failed) {
        data.clear();
        error = "could not read complete document";
        return false;
    }
    report_operation_progress(size, size);
    return true;
}

bool StdioFileSystem::read_range(const char* path,
                                 std::uint64_t offset,
                                 std::uint8_t* data,
                                 std::size_t size,
                                 std::string& error) {
    error.clear();
    if (path == nullptr || path[0] == '\0' || (data == nullptr && size != 0)) {
        error = "invalid ranged-read request";
        return false;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        error = std::string("could not open document: ") + std::strerror(errno);
        return false;
    }
    if (offset > static_cast<std::uint64_t>(LONG_MAX) ||
        std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
        std::fclose(file);
        error = "could not seek to document range";
        return false;
    }
    const std::size_t count = size == 0 ? 0 : std::fread(data, 1, size, file);
    const bool failed = count != size || std::ferror(file) != 0;
    const bool close_failed = std::fclose(file) != 0;
    if (failed || close_failed) {
        error = "could not read complete document range";
        return false;
    }
    return true;
}

bool StdioFileSystem::open_random_access(
    const char* path,
    std::shared_ptr<RandomAccessData>& source,
    std::string& error) {
    source.reset();
    error.clear();
    if (path == nullptr || path[0] == '\0') {
        error = "file path is empty";
        return false;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        error = std::string("could not open file: ") + std::strerror(errno);
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        error = "could not seek file";
        return false;
    }
    const long end = std::ftell(file);
    if (end < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        error = "could not determine file size";
        return false;
    }
    try {
        source = std::make_shared<StdioRandomAccessData>(
            file, static_cast<std::uint64_t>(end));
    } catch (...) {
        std::fclose(file);
        throw;
    }
    return true;
}

bool StdioFileSystem::write_atomic(const char* path,
                                   const std::uint8_t* data,
                                   std::size_t size,
                                   std::string& error) {
    error.clear();
    if (path == nullptr || path[0] == '\0' || (data == nullptr && size != 0)) {
        error = "invalid state-file write request";
        return false;
    }
    const std::string final_path(path);
    const std::string temporary = final_path + ".tmp";
    const std::string backup = final_path + ".bak";
    if (!write_complete_file(temporary, data, size, "temporary state file",
                             error)) {
        std::remove(temporary.c_str());
        return false;
    }

    std::remove(backup.c_str());
    errno = 0;
    bool had_previous =
        rename_for_replacement(final_path.c_str(), backup.c_str()) == 0;
    const int backup_error = errno;

    // When rename itself is unavailable, preserve the old state with a
    // verified copy before attempting the non-atomic rewrite below. This
    // gives that compatibility path the same rollback guarantee as the
    // normal final -> backup rename.
    if (!had_previous && replacement_rename_unavailable(backup_error)) {
        struct stat final_info {};
        if (stat(final_path.c_str(), &final_info) == 0) {
            if (!copy_file_verified(final_path, backup,
                                    "state-file backup", error)) {
                std::remove(temporary.c_str());
                return false;
            }
            had_previous = true;
        } else if (errno != ENOENT) {
            error = std::string("could not inspect existing state file: ") +
                    std::strerror(errno);
            std::remove(temporary.c_str());
            return false;
        }
    }

    const auto restore_backup = [&](std::string& rollback_error) {
        if (!had_previous) return true;
        // A failed direct rewrite can leave a partial destination behind.
        // Remove it before trying the cheapest rollback, then fall back to a
        // verified copy if rename is unavailable in this direction too.
        std::remove(final_path.c_str());
        errno = 0;
        if (rename_for_replacement(backup.c_str(), final_path.c_str()) == 0) {
            rollback_error.clear();
            return true;
        }
        if (!copy_file_verified(backup, final_path,
                                "state-file rollback", rollback_error)) {
            std::remove(final_path.c_str());
            return false;
        }
        std::remove(backup.c_str());
        rollback_error.clear();
        return true;
    };

    // TI-Nspire OS exposes the rename syscall through Ndless/newlib, but some
    // supported OS versions return ENOSYS for every call. The completed .tmp
    // file still proves the directory and payload are writable. In that case,
    // do the strongest available replacement: rewrite the destination, reopen
    // it, and compare every byte before deleting the validated temporary copy.
    const auto staged_rewrite = [&]() {
        std::string replacement_error;
        if (!write_complete_file(final_path, data, size,
                                 "replacement state file",
                                 replacement_error) ||
            !file_matches(final_path, data, size, replacement_error)) {
            // Keep the completed temporary file for recovery when validation
            // fails; it is safer than silently discarding the only good copy.
            error = std::move(replacement_error);
            if (had_previous) {
                std::string rollback_error;
                if (!restore_backup(rollback_error)) {
                    error += "; rollback failed: " + rollback_error;
                }
            }
            return false;
        }
        std::remove(temporary.c_str());
        if (had_previous) std::remove(backup.c_str());
        error.clear();
        return true;
    };

    if (replacement_rename_unavailable(backup_error)) {
        return staged_rewrite();
    }

    errno = 0;
    if (rename_for_replacement(temporary.c_str(), final_path.c_str()) != 0) {
        const int replacement_error = errno;
        if (replacement_rename_unavailable(replacement_error)) {
            return staged_rewrite();
        }
        if (had_previous) {
            std::string rollback_error;
            if (!restore_backup(rollback_error)) {
                error = std::string("could not replace state file: ") +
                        std::strerror(replacement_error) +
                        "; rollback failed: " + rollback_error;
                return false;
            }
        }
        std::remove(temporary.c_str());
        error = std::string("could not replace state file: ") +
                std::strerror(replacement_error);
        return false;
    }
    if (had_previous) std::remove(backup.c_str());
    return true;
}

bool StdioFileSystem::list_reader_documents(const char* root,
                                            std::size_t maximum_results,
                                            std::vector<std::string>& paths,
                                            std::string& error,
                                            bool* truncated) {
    paths.clear();
    error.clear();
    bool was_truncated = false;
    if (truncated != nullptr) *truncated = false;
    if (root == nullptr || root[0] == '\0' || maximum_results == 0) {
        error = "invalid document folder";
        return false;
    }
    std::size_t entries_remaining = kMaximumDocumentEntriesScanned;
    std::size_t entries_since_progress = 0;
    auto progress = [this]() { report_operation_progress(); };
    if (!collect_matching_files(root, 0, maximum_results, paths, error,
                                has_reader_extension, "document",
                                was_truncated, entries_remaining,
                                entries_since_progress, progress)) {
        paths.clear();
        return false;
    }
    if (truncated != nullptr) *truncated = was_truncated;
    std::sort(paths.begin(), paths.end(),
              [](const std::string& left, const std::string& right) {
                  return std::lexicographical_compare(
                      left.begin(), left.end(), right.begin(), right.end(),
                      [](unsigned char a, unsigned char b) {
                          return std::tolower(a) < std::tolower(b);
                      });
              });
    return true;
}

bool StdioFileSystem::list_font_files(const char* root,
                                      std::size_t maximum_results,
                                      std::vector<std::string>& paths,
                                      std::string& error,
                                      bool* truncated) {
    paths.clear();
    error.clear();
    bool was_truncated = false;
    if (truncated != nullptr) *truncated = false;
    if (root == nullptr || root[0] == '\0' || maximum_results == 0) {
        error = "invalid font folder";
        return false;
    }
    std::size_t entries_remaining = kMaximumFontEntriesScanned;
    std::size_t entries_since_progress = 0;
    auto progress = [this]() { report_operation_progress(); };
    if (!collect_matching_files(root, 0, maximum_results, paths, error,
                                has_font_file_extension, "font",
                                was_truncated, entries_remaining,
                                entries_since_progress, progress)) {
        paths.clear();
        return false;
    }
    if (truncated != nullptr) *truncated = was_truncated;
    std::sort(paths.begin(), paths.end(),
              [](const std::string& left, const std::string& right) {
                  return std::lexicographical_compare(
                      left.begin(), left.end(), right.begin(), right.end(),
                      [](unsigned char a, unsigned char b) {
                          return std::tolower(a) < std::tolower(b);
                      });
              });
    return true;
}

}  // namespace nmarkdown
