#include "feature.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "../../game/access/game_access.h"
#include "map_data.h"

namespace qol::features::custom_map {
namespace {

constexpr char kDefaultPackage[] =
    "com.com2us.inotia4.normal.freefull.google.global.android.common";
constexpr size_t kCmdlineBufferSize = 256;
constexpr size_t kCustomPathBufferSize = 1024;

std::atomic<const contracts::QolHostSdk*> g_sdk{nullptr};
std::atomic<bool> g_enabled{false};
std::atomic<ResLoadToPoolFn> g_original_loader{nullptr};

struct MapPathId {
    const char* digits = nullptr;
    size_t length = 0;
    uint32_t value = 0;
    bool value_valid = true;
};

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

bool parse_map_path(const char* path, MapPathId* out_id) {
    if (path == nullptr || out_id == nullptr || path[0] != 'm') return false;
    const char* digits = path + 1;
    const char* suffix = digits;
    while (*suffix >= '0' && *suffix <= '9') ++suffix;
    const size_t length = static_cast<size_t>(suffix - digits);
    if (length == 0 || length > static_cast<size_t>(INT_MAX) || std::strcmp(suffix, ".dat") != 0) {
        return false;
    }
    uint32_t value = 0;
    bool value_valid = true;
    for (size_t index = 0; index < length; ++index) {
        const uint32_t digit = static_cast<uint32_t>(digits[index] - '0');
        if (!value_valid) continue;
        if (value > (UINT32_MAX - digit) / 10) {
            value_valid = false;
        } else {
            value = value * 10 + digit;
        }
    }
    out_id->digits = digits;
    out_id->length = length;
    out_id->value = value;
    out_id->value_valid = value_valid;
    return true;
}

bool build_custom_path(const MapPathId& id, const char* suffix, char* out_path, size_t out_size) {
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

    // Package discovery follows infra/log/qol_log.cpp:245-253.
    const int written = std::snprintf(
        out_path, out_size,
        "/sdcard/Android/data/%s/files/qol/resources/map/m%.*s%s",
        package, static_cast<int>(id.length), id.digits, suffix);
    return written >= 0 && static_cast<size_t>(written) < out_size;
}

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

void log_loaded_map(const MapPathId& id, int32_t size) {
    const contracts::QolHostSdk* sdk = g_sdk.load(std::memory_order_acquire);
    if (sdk == nullptr || sdk->log.write == nullptr) return;
    char message[128]{};
    const int written = std::snprintf(message, sizeof(message),
                                      "custom map loaded id=%.*s size=%d",
                                      static_cast<int>(id.length), id.digits, size);
    if (written < 0) return;
    const uint32_t length = static_cast<uint32_t>(written) < sizeof(message)
        ? static_cast<uint32_t>(written)
        : static_cast<uint32_t>(sizeof(message) - 1);
    (void)sdk->log.write(1, 1, "plugins/custom_map", message, length);
}

void log_conversion(const MapPathId& id, const char* status,
                   const CustomMapDecodeDiagnostics& diagnostics = {}) {
    const contracts::QolHostSdk* sdk = g_sdk.load(std::memory_order_acquire);
    if (sdk == nullptr || sdk->log.write == nullptr) return;
    char message[256]{};
    const int written = std::snprintf(message, sizeof(message),
        "custom map jpg conversion id=%.*s status=%s reason=%s input=%zu return=%d output=%zu",
        static_cast<int>(id.length), id.digits, status,
        diagnostics.failure_reason == nullptr ? "unknown" : diagnostics.failure_reason,
        diagnostics.input_bytes, diagnostics.decoder_status, diagnostics.output_bytes);
    if (written < 0) return;
    const uint32_t length = static_cast<uint32_t>(written) < sizeof(message)
        ? static_cast<uint32_t>(written) : static_cast<uint32_t>(sizeof(message) - 1);
    (void)sdk->log.write(1, 1, "plugins/custom_map", message, length);
}

bool load_custom_dat(const MapPathId& id, const char* path, void** out_buf, int32_t* out_size) {
    FileDescriptor fd(open(path, O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) return false;

    struct stat file_stat {};
    if (fstat(fd.get(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
        file_stat.st_size < 0 || file_stat.st_size > INT32_MAX) {
        return false;
    }

    const int32_t file_size = static_cast<int32_t>(file_stat.st_size);
    const size_t allocation_size = file_size == 0 ? 1 : static_cast<size_t>(file_size);
    void* buffer = fn_mem_malloc(allocation_size);
    if (buffer == nullptr) return false;

    if (!read_exact(fd.get(), buffer, static_cast<size_t>(file_size))) {
        fn_mem_free(buffer);
        return false;
    }

    *out_buf = buffer;
    if (out_size != nullptr) *out_size = file_size;
    log_loaded_map(id, file_size);
    return true;
}

bool load_custom_jpg(const MapPathId& id, const char* dat_path, const char* jpg_path,
                     void** out_buf, int32_t* out_size) {
    struct stat jpg_stat {};
    if (stat(jpg_path, &jpg_stat) != 0) {
        if (errno != ENOENT) log_conversion(id, "source_stat_failed");
        return false;
    }
    if (!S_ISREG(jpg_stat.st_mode)) {
        log_conversion(id, "source_not_regular");
        return false;
    }

    std::vector<uint8_t> decoded;
    CustomMapDecodeDiagnostics diagnostics{};
    const auto decoded_result = custom_map_decode_jpg(jpg_path, &decoded, &diagnostics);
    if (decoded_result != CustomMapConversionResult::kConverted || decoded.empty() ||
        decoded.size() > static_cast<size_t>(INT32_MAX)) {
        const char* status = "decode_failed";
        if (decoded_result == CustomMapConversionResult::kInvalidContainer) {
            status = "invalid_container";
        } else if (decoded_result == CustomMapConversionResult::kReadFailed) {
            status = "read_failed";
        } else if (decoded_result == CustomMapConversionResult::kInvalidMapData) {
            status = "invalid_map_data";
        }
        log_conversion(id, status, diagnostics);
        return false;
    }

    void* buffer = fn_mem_malloc(decoded.size());
    if (buffer == nullptr) {
        diagnostics.failure_reason = "game_allocation_failed";
        log_conversion(id, "game_allocation_failed", diagnostics);
        return false;
    }
    std::memcpy(buffer, decoded.data(), decoded.size());

    const auto persisted = custom_map_write_decoded_dat(
        jpg_path, dat_path, id.value, decoded, nullptr);
    if (persisted == CustomMapConversionResult::kDatAlreadyExists) {
        fn_mem_free(buffer);
        diagnostics.failure_reason = "dat_already_exists";
        log_conversion(id, "dat_already_exists", diagnostics);
        return load_custom_dat(id, dat_path, out_buf, out_size);
    }
    if (persisted == CustomMapConversionResult::kInvalidMapData) {
        fn_mem_free(buffer);
        diagnostics.failure_reason = "map_structure_invalid_before_write";
        log_conversion(id, "invalid_map_data", diagnostics);
        return false;
    }
    if (persisted == CustomMapConversionResult::kConverted) {
        log_conversion(id, "converted", diagnostics);
    } else {
        // A valid in-memory payload still serves this map load when persistence fails.
        diagnostics.failure_reason = "write_failed_using_memory";
        log_conversion(id, "write_failed_using_memory", diagnostics);
    }

    *out_buf = buffer;
    if (out_size != nullptr) *out_size = static_cast<int32_t>(decoded.size());
    log_loaded_map(id, static_cast<int32_t>(decoded.size()));
    return true;
}

bool load_custom_map(const MapPathId& id, void** out_buf, int32_t* out_size) {
    if (fn_mem_malloc == nullptr || fn_mem_free == nullptr || out_buf == nullptr) return false;

    char dat_path[kCustomPathBufferSize]{};
    char jpg_path[kCustomPathBufferSize]{};
    if (!build_custom_path(id, ".dat", dat_path, sizeof(dat_path)) ||
        !build_custom_path(id, ".dat.jpg", jpg_path, sizeof(jpg_path))) return false;

    struct stat dat_stat {};
    if (stat(dat_path, &dat_stat) == 0) {
        // Existing .dat always wins, even when it cannot be read.
        return load_custom_dat(id, dat_path, out_buf, out_size);
    }
    if (errno != ENOENT) return false;
    if (!id.value_valid) {
        log_conversion(id, "map_id_out_of_range");
        return false;
    }
    return load_custom_jpg(id, dat_path, jpg_path, out_buf, out_size);
}

}  // namespace

void initialize(const contracts::QolHostSdk* sdk) {
    g_enabled.store(false, std::memory_order_release);
    g_original_loader.store(nullptr, std::memory_order_release);
    g_sdk.store(sdk, std::memory_order_release);
}

void clear_runtime() {
    g_enabled.store(false, std::memory_order_release);
    g_original_loader.store(nullptr, std::memory_order_release);
    g_sdk.store(nullptr, std::memory_order_release);
}

void set_enabled(bool value) {
    g_enabled.store(value, std::memory_order_release);
}

bool enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void set_original_loader(ResLoadToPoolFn loader) {
    g_original_loader.store(loader, std::memory_order_release);
}

int32_t load_resource(char* path, void** out_buf, int32_t* out_size) {
    const ResLoadToPoolFn original = g_original_loader.load(std::memory_order_acquire);
    if (original == nullptr) return -1;
    if (!enabled()) return original(path, out_buf, out_size);

    MapPathId id{};
    if (!parse_map_path(path, &id)) return original(path, out_buf, out_size);
    if (!load_custom_map(id, out_buf, out_size)) return original(path, out_buf, out_size);
    return 0;
}

}  // namespace qol::features::custom_map
