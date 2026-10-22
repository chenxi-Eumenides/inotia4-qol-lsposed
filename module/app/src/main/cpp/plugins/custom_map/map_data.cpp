#include "map_data.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <new>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "../../game/access/game_access.h"
#include "../../game/access/symbols.h"

namespace qol::features::custom_map {
namespace {

constexpr char kDefaultPackage[] =
    "com.com2us.inotia4.normal.freefull.google.global.android.common";
constexpr size_t kCmdlineBufferSize = 256;
constexpr size_t kPathBufferSize = 1024;

class FileDescriptor {
public:
    explicit FileDescriptor(int fd) : fd_(fd) {}
    ~FileDescriptor() {
        if (fd_ >= 0) close(fd_);
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const { return fd_; }

private:
    int fd_;
};

class Directory {
public:
    explicit Directory(DIR* dir) : dir_(dir) {}
    ~Directory() {
        if (dir_ != nullptr) closedir(dir_);
    }

    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;

    DIR* get() const { return dir_; }

private:
    DIR* dir_;
};

struct GameBufferDeleter {
    MemFreeFn free = nullptr;

    void operator()(void* buffer) const {
        if (buffer != nullptr && free != nullptr) free(buffer);
    }
};

using GameBuffer = std::unique_ptr<void, GameBufferDeleter>;
using LzmaDecodeFn = int32_t (*)(void*, int32_t, void**, int32_t*);

bool read_exact(int fd, void* data, size_t size) {
    auto* cursor = static_cast<uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        const ssize_t count = read(fd, cursor, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        cursor += static_cast<size_t>(count);
        remaining -= static_cast<size_t>(count);
    }
    return true;
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    if (out == nullptr) return false;
    FileDescriptor fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) return false;

    struct stat file_stat {};
    if (fstat(fd.get(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
        file_stat.st_size < 0 || file_stat.st_size > INT32_MAX) {
        return false;
    }

    std::vector<uint8_t> contents;
    contents.resize(static_cast<size_t>(file_stat.st_size));
    if (!read_exact(fd.get(), contents.data(), contents.size())) return false;
    *out = std::move(contents);
    return true;
}

bool build_map_directory(std::string* out) {
    if (out == nullptr) return false;

    char cmdline[kCmdlineBufferSize]{};
    const int cmdline_fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    const ssize_t bytes_read = cmdline_fd >= 0
        ? read(cmdline_fd, cmdline, sizeof(cmdline) - 1)
        : -1;
    if (cmdline_fd >= 0) close(cmdline_fd);

    const char* package = kDefaultPackage;
    if (bytes_read > 0) {
        cmdline[static_cast<size_t>(bytes_read)] = '\0';
        package = cmdline;
    }

    char path[kPathBufferSize]{};
    const int written = std::snprintf(
        path, sizeof(path), "/sdcard/Android/data/%s/files/qol/resources/map", package);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(path)) return false;
    *out = path;
    return true;
}

bool parse_map_filename(const char* filename, uint32_t* out_id) {
    if (filename == nullptr || out_id == nullptr || filename[0] != 'm') return false;

    const char* digits = filename + 1;
    const char* suffix = digits;
    uint32_t id = 0;
    while (*suffix >= '0' && *suffix <= '9') {
        const uint32_t digit = static_cast<uint32_t>(*suffix - '0');
        if (id > (UINT32_MAX - digit) / 10) return false;
        id = id * 10 + digit;
        ++suffix;
    }
    if (suffix == digits || std::strcmp(suffix, ".dat") != 0) return false;
    *out_id = id;
    return true;
}

bool list_map_files(std::vector<CustomMapFile>* out_files) {
    if (out_files == nullptr) return false;
    std::string directory_path;
    if (!build_map_directory(&directory_path)) return false;

    Directory directory(opendir(directory_path.c_str()));
    if (directory.get() == nullptr) return false;

    std::vector<CustomMapFile> files;
    for (;;) {
        errno = 0;
        dirent* entry = readdir(directory.get());
        if (entry == nullptr) {
            if (errno != 0) return false;
            break;
        }

        uint32_t id = 0;
        if (!parse_map_filename(entry->d_name, &id)) continue;
        const std::string name(entry->d_name);
        const std::string path = directory_path + "/" + name;
        struct stat file_stat {};
        if (stat(path.c_str(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
            file_stat.st_size < 0) continue;
        files.push_back(CustomMapFile{id, name, path,
            static_cast<uint64_t>(file_stat.st_size),
            static_cast<int64_t>(file_stat.st_mtim.tv_sec),
            static_cast<int64_t>(file_stat.st_mtim.tv_nsec)});
    }

    std::sort(files.begin(), files.end(), [](const CustomMapFile& left, const CustomMapFile& right) {
        if (left.id != right.id) return left.id < right.id;
        return left.name < right.name;
    });
    *out_files = std::move(files);
    return true;
}

bool has_bytes(size_t data_size, size_t offset, size_t count) {
    return offset <= data_size && count <= data_size - offset;
}

uint16_t read_u16_le(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

bool parse_map_data(uint32_t id, const uint8_t* data, size_t data_size,
                    CustomMapDetail* out_detail) {
    if (data == nullptr || data_size < 4 || out_detail == nullptr) return false;

    const uint32_t width = data[2];
    const uint32_t height = data[3];
    const size_t tile_count = static_cast<size_t>(width) * height;
    const size_t tile_bytes = tile_count * 2;
    size_t offset = 4;
    if (!has_bytes(data_size, offset, tile_bytes)) return false;

    CustomMapDetail detail{};
    detail.id = id;
    detail.width = width;
    detail.height = height;
    detail.base_tiles.reserve(tile_count);
    for (size_t index = 0; index < tile_count; ++index) {
        const uint8_t first = data[offset++];
        const uint8_t second = data[offset++];
        detail.base_tiles.push_back(static_cast<uint16_t>(((first & 0x7u) << 8) | second));
    }

    if (!has_bytes(data_size, offset, 11)) return false;
    offset += 10;
    const uint8_t section_count = data[offset++];
    for (uint32_t section = 0; section < section_count; ++section) {
        if (!has_bytes(data_size, offset, 3)) return false;
        const uint16_t feature_count = read_u16_le(data + offset + 1);
        offset += 3;
        const size_t feature_bytes = static_cast<size_t>(feature_count) * 4;
        if (!has_bytes(data_size, offset, feature_bytes)) return false;
        offset += feature_bytes;
    }

    if (!has_bytes(data_size, offset, 1)) return false;
    const uint8_t exit_count = data[offset++];
    const size_t exit_bytes = static_cast<size_t>(exit_count) * 6;
    if (!has_bytes(data_size, offset, exit_bytes)) return false;
    for (uint32_t exit_index = 0; exit_index < exit_count; ++exit_index) {
        const uint8_t* encoded_exit = data + offset;
        detail.exits.push_back(CustomMapExit{
            encoded_exit[0], encoded_exit[1], encoded_exit[2], encoded_exit[3],
            read_u16_le(encoded_exit + 4),
        });
        offset += 6;
    }

    *out_detail = std::move(detail);
    return true;
}

bool read_map_payload(const std::string& path, std::vector<uint8_t>* file_data,
                      GameBuffer* decoded, const uint8_t** out_data, size_t* out_size) {
    if (file_data == nullptr || decoded == nullptr || out_data == nullptr || out_size == nullptr ||
        !read_file(path, file_data) || file_data->empty()) {
        return false;
    }

    if ((*file_data)[0] != 0x01) {
        *out_data = file_data->data();
        *out_size = file_data->size();
        return true;
    }
    if (file_data->size() <= 1 || file_data->size() - 1 > INT32_MAX) return false;

    const MemFreeFn free_buffer = fn_mem_free;
    if (free_buffer == nullptr) return false;
    const uintptr_t decode_address = qol::game::resolve_hook_entry("F_LZMA_DECODE_VMA", nullptr);
    if (decode_address == 0) return false;

    const auto decode = reinterpret_cast<LzmaDecodeFn>(decode_address);
    void* decoded_buffer = nullptr;
    int32_t decoded_size = 0;
    GameBuffer decoded_guard(nullptr, GameBufferDeleter{free_buffer});
    const int32_t succeeded = decode(file_data->data() + 1,
                                    static_cast<int32_t>(file_data->size()),
                                    &decoded_buffer, &decoded_size);
    decoded_guard.reset(decoded_buffer);
    if (succeeded == 0 || decoded_buffer == nullptr || decoded_size <= 0) return false;

    *out_data = static_cast<const uint8_t*>(decoded_buffer);
    *out_size = static_cast<size_t>(decoded_size);
    *decoded = std::move(decoded_guard);
    return true;
}

bool file_stamp_matches(const CustomMapFile& file) {
    struct stat current {};
    return stat(file.path.c_str(), &current) == 0 && S_ISREG(current.st_mode) &&
        current.st_size >= 0 && static_cast<uint64_t>(current.st_size) == file.size &&
        static_cast<int64_t>(current.st_mtim.tv_sec) == file.modified_sec &&
        static_cast<int64_t>(current.st_mtim.tv_nsec) == file.modified_nsec;
}

}  // namespace

bool custom_map_list_files(std::vector<CustomMapFile>* out) {
    if (out == nullptr) return false;
    try {
        std::vector<CustomMapFile> files;
        if (!list_map_files(&files)) return false;
        for (size_t index = 1; index < files.size(); ++index)
            if (files[index - 1].id == files[index].id) return false;
        *out = std::move(files);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool custom_map_file_unchanged(const CustomMapFile& file) {
    return file_stamp_matches(file);
}

bool custom_map_read_summary(const CustomMapFile& file, CustomMapSummary* out) {
    if (out == nullptr || !file_stamp_matches(file)) return false;
    try {
        std::vector<uint8_t> file_data;
        GameBuffer decoded(nullptr, GameBufferDeleter{});
        const uint8_t* map_data = nullptr;
        size_t map_size = 0;
        if (!read_map_payload(file.path, &file_data, &decoded, &map_data, &map_size)) return false;
        if (map_data == nullptr || map_size < 4) return false;
        const uint32_t width = map_data[2];
        const uint32_t height = map_data[3];
        const size_t tile_bytes = static_cast<size_t>(width) * height * 2;
        if (!has_bytes(map_size, 4, tile_bytes) || !file_stamp_matches(file)) return false;
        const CustomMapSummary summary{file.id, width, height};
        *out = summary;
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

bool custom_map_read_detail(const CustomMapFile& file, CustomMapDetail* out) {
    if (out == nullptr) return false;
    if (!file_stamp_matches(file)) return false;
    try {
        std::vector<uint8_t> file_data;
        GameBuffer decoded(nullptr, GameBufferDeleter{});
        const uint8_t* map_data = nullptr;
        size_t map_size = 0;
        if (!read_map_payload(file.path, &file_data, &decoded, &map_data, &map_size)) return false;

        CustomMapDetail detail{};
        if (!parse_map_data(file.id, map_data, map_size, &detail) ||
            !file_stamp_matches(file)) return false;
        *out = std::move(detail);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

}  // namespace qol::features::custom_map
