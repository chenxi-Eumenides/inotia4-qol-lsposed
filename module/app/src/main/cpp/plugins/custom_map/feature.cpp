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

#include "../../game/access/game_access.h"

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
    out_id->digits = digits;
    out_id->length = length;
    return true;
}

bool build_custom_path(const MapPathId& id, char* out_path, size_t out_size) {
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
        "/sdcard/Android/data/%s/files/qol/resources/map/m%.*s.dat",
        package, static_cast<int>(id.length), id.digits);
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

bool load_custom_map(const MapPathId& id, void** out_buf, int32_t* out_size) {
    if (fn_mem_malloc == nullptr || fn_mem_free == nullptr || out_buf == nullptr) return false;

    char path[kCustomPathBufferSize]{};
    if (!build_custom_path(id, path, sizeof(path))) return false;

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
