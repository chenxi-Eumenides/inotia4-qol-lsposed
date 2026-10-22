#include "map_data.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <map>
#include <new>
#include <string>
#include <set>
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
    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this == &other) return *this;
        if (fd_ >= 0) close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
        return *this;
    }

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

bool parse_map_jpg_filename(const char* filename, uint32_t* out_id) {
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
    if (suffix == digits || std::strcmp(suffix, ".dat.jpg") != 0) return false;
    *out_id = id;
    return true;
}

bool list_map_files(std::vector<CustomMapFile>* out_files) {
    if (out_files == nullptr) return false;
    std::string directory_path;
    if (!build_map_directory(&directory_path)) return false;

    Directory directory(opendir(directory_path.c_str()));
    if (directory.get() == nullptr) return false;

    std::vector<CustomMapFile> dat_files;
    std::vector<CustomMapFile> jpg_files;
    std::set<uint32_t> dat_ids;
    for (;;) {
        errno = 0;
        dirent* entry = readdir(directory.get());
        if (entry == nullptr) {
            if (errno != 0) return false;
            break;
        }

        uint32_t id = 0;
        CustomMapFileKind kind{};
        if (parse_map_filename(entry->d_name, &id)) {
            kind = CustomMapFileKind::kDat;
            dat_ids.insert(id);
        } else if (parse_map_jpg_filename(entry->d_name, &id)) {
            kind = CustomMapFileKind::kDatJpg;
        } else {
            continue;
        }
        const std::string name(entry->d_name);
        const std::string path = directory_path + "/" + name;
        struct stat file_stat {};
        if (stat(path.c_str(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
            file_stat.st_size < 0) continue;
        CustomMapFile file{id, name, path,
            static_cast<uint64_t>(file_stat.st_size),
            static_cast<int64_t>(file_stat.st_mtim.tv_sec),
            static_cast<int64_t>(file_stat.st_mtim.tv_nsec), kind};
        (kind == CustomMapFileKind::kDat ? dat_files : jpg_files).push_back(std::move(file));
    }

    const auto by_id_then_name = [](const CustomMapFile& left, const CustomMapFile& right) {
        if (left.id != right.id) return left.id < right.id;
        return left.name < right.name;
    };
    std::sort(dat_files.begin(), dat_files.end(), by_id_then_name);
    std::sort(jpg_files.begin(), jpg_files.end(), by_id_then_name);

    std::map<uint32_t, CustomMapFile> jpg_by_id;
    for (auto& file : jpg_files) {
        if (dat_ids.find(file.id) != dat_ids.end()) continue;
        auto existing = jpg_by_id.find(file.id);
        if (existing == jpg_by_id.end()) {
            jpg_by_id.emplace(file.id, std::move(file));
            continue;
        }
        char canonical[48]{};
        const int written = std::snprintf(canonical, sizeof(canonical), "m%u.dat.jpg", file.id);
        if (written >= 0 && static_cast<size_t>(written) < sizeof(canonical) &&
            file.name == canonical) {
            existing->second = std::move(file);
        }
    }
    std::vector<CustomMapFile> files;
    files.reserve(dat_files.size() + jpg_by_id.size());
    for (size_t index = 0; index < dat_files.size(); ++index) {
        if (index > 0 && dat_files[index - 1].id == dat_files[index].id) return false;
        files.push_back(std::move(dat_files[index]));
    }
    for (auto& [id, file] : jpg_by_id) {
        (void)id;
        files.push_back(std::move(file));
    }
    std::sort(files.begin(), files.end(), by_id_then_name);
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
    if (width == 0 || height == 0) return false;
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

bool decode_container(std::vector<uint8_t>& file_data, GameBuffer* decoded,
                      const uint8_t** out_data, size_t* out_size,
                      CustomMapDecodeDiagnostics* diagnostics) {
    if (decoded == nullptr || out_data == nullptr || out_size == nullptr) return false;
    if (file_data.size() <= 1 || file_data.size() - 1 > INT32_MAX || file_data[0] != 0x01) {
        if (diagnostics != nullptr) diagnostics->failure_reason = "invalid_container";
        return false;
    }
    const MemFreeFn free_buffer = fn_mem_free;
    if (free_buffer == nullptr) {
        if (diagnostics != nullptr) diagnostics->failure_reason = "mem_free_unavailable";
        return false;
    }
    const uintptr_t decode_address = qol::game::resolve_hook_entry("F_LZMA_DECODE_VMA", nullptr);
    if (decode_address == 0) {
        if (diagnostics != nullptr) diagnostics->failure_reason = "decoder_unavailable";
        return false;
    }

    const auto decode = reinterpret_cast<LzmaDecodeFn>(decode_address);
    void* decoded_buffer = nullptr;
    int32_t decoded_size = 0;
    GameBuffer decoded_guard(nullptr, GameBufferDeleter{free_buffer});
    const int32_t succeeded = decode(file_data.data() + 1,
                                    static_cast<int32_t>(file_data.size() - 1),
                                    &decoded_buffer, &decoded_size);
    if (diagnostics != nullptr) {
        diagnostics->decoder_status = succeeded;
        diagnostics->output_bytes = decoded_size > 0 ? static_cast<size_t>(decoded_size) : 0;
    }
    decoded_guard.reset(decoded_buffer);
    if (succeeded == 0) {
        if (diagnostics != nullptr) diagnostics->failure_reason = "decoder_rejected_input";
        return false;
    }
    if (decoded_buffer == nullptr || decoded_size <= 0) {
        if (diagnostics != nullptr) diagnostics->failure_reason = "decoder_output_invalid";
        return false;
    }

    *out_data = static_cast<const uint8_t*>(decoded_buffer);
    *out_size = static_cast<size_t>(decoded_size);
    *decoded = std::move(decoded_guard);
    return true;
}

bool read_map_payload(const std::string& path, std::vector<uint8_t>* file_data,
                      GameBuffer* decoded, const uint8_t** out_data, size_t* out_size) {
    if (file_data == nullptr || decoded == nullptr || out_data == nullptr || out_size == nullptr ||
        !read_file(path, file_data) || file_data->empty()) return false;
    if ((*file_data)[0] != 0x01) {
        *out_data = file_data->data();
        *out_size = file_data->size();
        return true;
    }
    return decode_container(*file_data, decoded, out_data, out_size, nullptr);
}

bool file_stamp_matches(const CustomMapFile& file) {
    struct stat current {};
    return stat(file.path.c_str(), &current) == 0 && S_ISREG(current.st_mode) &&
        current.st_size >= 0 && static_cast<uint64_t>(current.st_size) == file.size &&
        static_cast<int64_t>(current.st_mtim.tv_sec) == file.modified_sec &&
        static_cast<int64_t>(current.st_mtim.tv_nsec) == file.modified_nsec;
}

std::atomic<uint32_t> g_temp_sequence{0};

bool write_exact(int fd, const uint8_t* data, size_t size) {
    size_t remaining = size;
    const uint8_t* cursor = data;
    while (remaining > 0) {
        const ssize_t written = write(fd, cursor, remaining);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        cursor += static_cast<size_t>(written);
        remaining -= static_cast<size_t>(written);
    }
    return true;
}

bool snapshot_dat_file(uint32_t id, const std::string& path, CustomMapFile* out) {
    if (out == nullptr) return false;
    struct stat file_stat {};
    if (stat(path.c_str(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
        file_stat.st_size < 0) return false;
    const size_t slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    *out = CustomMapFile{id, name, path, static_cast<uint64_t>(file_stat.st_size),
        static_cast<int64_t>(file_stat.st_mtim.tv_sec),
        static_cast<int64_t>(file_stat.st_mtim.tv_nsec), CustomMapFileKind::kDat};
    return true;
}

CustomMapConversionResult write_decoded_dat_internal(
    const std::string& dat_path, uint32_t id, const std::vector<uint8_t>& decoded,
    CustomMapFile* out_dat) {
    if (decoded.empty()) return CustomMapConversionResult::kDecodeFailed;
    const bool prefix_zero = decoded[0] == 1;
    if (decoded.size() > static_cast<size_t>(INT32_MAX) - (prefix_zero ? 1u : 0u))
        return CustomMapConversionResult::kWriteFailed;
    std::vector<uint8_t> normalized;
    const uint8_t* map_data = decoded.data();
    size_t map_size = decoded.size();
    if (prefix_zero) {
        normalized.reserve(decoded.size() + 1);
        normalized.push_back(0);
        normalized.insert(normalized.end(), decoded.begin(), decoded.end());
        map_data = normalized.data();
        map_size = normalized.size();
    }
    CustomMapDetail detail{};
    if (!parse_map_data(id, map_data, map_size, &detail))
        return CustomMapConversionResult::kInvalidMapData;

    struct stat existing {};
    if (stat(dat_path.c_str(), &existing) == 0) {
        if (out_dat != nullptr && !snapshot_dat_file(id, dat_path, out_dat))
            return CustomMapConversionResult::kWriteFailed;
        return CustomMapConversionResult::kDatAlreadyExists;
    }
    if (errno != ENOENT) return CustomMapConversionResult::kWriteFailed;

    std::string temporary;
    FileDescriptor fd(-1);
    for (int attempt = 0; attempt < 8; ++attempt) {
        char suffix[64]{};
        const uint32_t sequence = g_temp_sequence.fetch_add(1, std::memory_order_relaxed);
        const int written = std::snprintf(suffix, sizeof(suffix), ".tmp.%d.%u",
                                          static_cast<int>(getpid()), sequence);
        if (written < 0 || static_cast<size_t>(written) >= sizeof(suffix))
            return CustomMapConversionResult::kWriteFailed;
        temporary = dat_path + suffix;
        const int opened = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (opened >= 0) {
            fd = FileDescriptor(opened);
            break;
        }
        if (errno != EEXIST) return CustomMapConversionResult::kWriteFailed;
    }
    if (fd.get() < 0) return CustomMapConversionResult::kWriteFailed;

    const uint8_t leading_zero = 0;
    if ((prefix_zero && !write_exact(fd.get(), &leading_zero, 1)) ||
        !write_exact(fd.get(), decoded.data(), decoded.size()) || fsync(fd.get()) != 0) {
        unlink(temporary.c_str());
        return CustomMapConversionResult::kWriteFailed;
    }
    struct stat temp_stat {};
    if (fstat(fd.get(), &temp_stat) != 0 || !S_ISREG(temp_stat.st_mode) ||
        temp_stat.st_size < 0) {
        unlink(temporary.c_str());
        return CustomMapConversionResult::kWriteFailed;
    }
    CustomMapFile output_record{};
    if (out_dat != nullptr) {
        const size_t slash = dat_path.find_last_of('/');
        output_record = CustomMapFile{id,
            slash == std::string::npos ? dat_path : dat_path.substr(slash + 1), dat_path,
            static_cast<uint64_t>(temp_stat.st_size),
            static_cast<int64_t>(temp_stat.st_mtim.tv_sec),
            static_cast<int64_t>(temp_stat.st_mtim.tv_nsec), CustomMapFileKind::kDat};
    }

    if (link(temporary.c_str(), dat_path.c_str()) != 0) {
        const int link_error = errno;
        unlink(temporary.c_str());
        if (link_error == EEXIST &&
            (out_dat == nullptr || snapshot_dat_file(id, dat_path, out_dat)))
            return CustomMapConversionResult::kDatAlreadyExists;
        return CustomMapConversionResult::kWriteFailed;
    }
    (void)unlink(temporary.c_str());
    if (out_dat != nullptr) *out_dat = std::move(output_record);
    return CustomMapConversionResult::kConverted;
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

CustomMapConversionResult custom_map_decode_jpg(const std::string& path,
                                                 std::vector<uint8_t>* out_decoded,
                                                 CustomMapDecodeDiagnostics* diagnostics) {
    if (out_decoded == nullptr) return CustomMapConversionResult::kReadFailed;
    out_decoded->clear();
    if (diagnostics != nullptr) *diagnostics = CustomMapDecodeDiagnostics{};
    try {
        std::vector<uint8_t> source;
        if (!read_file(path, &source)) {
            if (diagnostics != nullptr) diagnostics->failure_reason = "read_failed";
            return CustomMapConversionResult::kReadFailed;
        }
        if (diagnostics != nullptr) diagnostics->input_bytes = source.size();
        if (source.empty() || source[0] != 1) {
            if (diagnostics != nullptr) diagnostics->failure_reason = "invalid_container";
            return CustomMapConversionResult::kInvalidContainer;
        }

        GameBuffer decoded(nullptr, GameBufferDeleter{});
        const uint8_t* decoded_data = nullptr;
        size_t decoded_size = 0;
        if (!decode_container(source, &decoded, &decoded_data, &decoded_size, diagnostics) ||
            decoded_data == nullptr || decoded_size == 0) {
            return CustomMapConversionResult::kDecodeFailed;
        }
        const bool prefix_zero = decoded_data[0] == 1;
        if (decoded_size > static_cast<size_t>(INT32_MAX) - (prefix_zero ? 1u : 0u)) {
            if (diagnostics != nullptr) diagnostics->failure_reason = "decoded_size_too_large";
            return CustomMapConversionResult::kDecodeFailed;
        }
        std::vector<uint8_t> result;
        result.reserve(decoded_size + (prefix_zero ? 1u : 0u));
        if (prefix_zero) result.push_back(0);
        result.insert(result.end(), decoded_data, decoded_data + decoded_size);
        CustomMapDetail detail{};
        if (!parse_map_data(0, result.data(), result.size(), &detail)) {
            if (diagnostics != nullptr) diagnostics->failure_reason = "map_structure_invalid";
            return CustomMapConversionResult::kInvalidMapData;
        }
        *out_decoded = std::move(result);
        if (diagnostics != nullptr) diagnostics->failure_reason = "none";
        return CustomMapConversionResult::kConverted;
    } catch (const std::bad_alloc&) {
        if (diagnostics != nullptr) diagnostics->failure_reason = "allocation_failed";
        return CustomMapConversionResult::kDecodeFailed;
    } catch (...) {
        if (diagnostics != nullptr) diagnostics->failure_reason = "decode_exception";
        return CustomMapConversionResult::kDecodeFailed;
    }
}

CustomMapConversionResult custom_map_write_decoded_dat(
    const std::string& jpg_path, const std::string& dat_path, uint32_t id,
    const std::vector<uint8_t>& decoded, CustomMapFile* out_dat) {
    if (jpg_path.empty() || dat_path.empty()) return CustomMapConversionResult::kWriteFailed;
    try {
        return write_decoded_dat_internal(dat_path, id, decoded, out_dat);
    } catch (const std::bad_alloc&) {
        return CustomMapConversionResult::kWriteFailed;
    }
}

CustomMapConversionResult custom_map_convert_jpg(const CustomMapFile& file,
                                                  CustomMapFile* out_dat,
                                                  CustomMapDecodeDiagnostics* diagnostics) {
    if (diagnostics != nullptr) *diagnostics = CustomMapDecodeDiagnostics{};
    if (file.kind != CustomMapFileKind::kDatJpg || !file_stamp_matches(file)) {
        if (diagnostics != nullptr) diagnostics->failure_reason = "source_changed";
        return CustomMapConversionResult::kSourceChanged;
    }
    std::vector<uint8_t> decoded;
    const CustomMapConversionResult decode_result =
        custom_map_decode_jpg(file.path, &decoded, diagnostics);
    if (decode_result != CustomMapConversionResult::kConverted) return decode_result;
    if (!file_stamp_matches(file)) {
        if (diagnostics != nullptr) diagnostics->failure_reason = "source_changed";
        return CustomMapConversionResult::kSourceChanged;
    }

    const size_t slash = file.path.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "" : file.path.substr(0, slash + 1);
    char dat_name[32]{};
    const int written = std::snprintf(dat_name, sizeof(dat_name), "m%u.dat", file.id);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(dat_name)) {
        if (diagnostics != nullptr) diagnostics->failure_reason = "dat_name_invalid";
        return CustomMapConversionResult::kWriteFailed;
    }
    const CustomMapConversionResult write_result =
        custom_map_write_decoded_dat(file.path, directory + dat_name, file.id, decoded, out_dat);
    if (diagnostics != nullptr && write_result != CustomMapConversionResult::kConverted &&
        write_result != CustomMapConversionResult::kDatAlreadyExists) {
        diagnostics->failure_reason = write_result == CustomMapConversionResult::kInvalidMapData
            ? "map_structure_invalid_before_write" : "write_failed";
    }
    return write_result;
}

bool custom_map_read_summary(const CustomMapFile& file, CustomMapSummary* out) {
    if (out == nullptr || file.kind != CustomMapFileKind::kDat || !file_stamp_matches(file))
        return false;
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
    if (out == nullptr || file.kind != CustomMapFileKind::kDat) return false;
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
