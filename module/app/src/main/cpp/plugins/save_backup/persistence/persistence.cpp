#include "persistence.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>

#include "plugins/save_backup/bundle.h"

namespace save_backup {
namespace persistence {
namespace detail {

std::atomic<uint64_t> g_file_seq{0};
#if defined(QOL_STORE_TEST_SEAMS)
std::string g_atomic_write_failure_path;
#endif

std::string next_file_suffix() {
    return std::to_string(g_file_seq.fetch_add(1, std::memory_order_relaxed) + 1);
}

}  // namespace detail

using detail::g_file_seq;
using detail::next_file_suffix;

bool ensure_dir(const std::string& path) {
    struct stat st{};
    // lstat：已存在的 symlink 不是目录，不能当作可写目录使用。
    if (::lstat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    if (errno != ENOENT) return false;
    return ::mkdir(path.c_str(), 0755) == 0;
}

bool file_exists(const std::string& path) {
    struct stat st{};
    // symlink 视为不存在，避免通过链接读到/删到目标目录之外。
    return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        ::close(fd);
        return false;
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(st.st_size));
    uint8_t buf[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        bytes.insert(bytes.end(), buf, buf + n);
    }
    if (::close(fd) != 0) return false;
    out.swap(bytes);
    return true;
}

// 原子写：写 <path>.tmp-<id> 再 rename；临时文件用 O_EXCL 创建，避免复用残留路径。
bool write_file_bytes_atomic(const std::string& path, const uint8_t* data, size_t size) {
#if defined(QOL_STORE_TEST_SEAMS)
    if (!detail::g_atomic_write_failure_path.empty() &&
        detail::g_atomic_write_failure_path == path) {
        detail::g_atomic_write_failure_path.clear();
        return false;
    }
#endif
    const std::string tmp = path + ".tmp-" + next_file_suffix();
    const int fd = ::open(tmp.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd, data + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (written == 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

#if defined(QOL_STORE_TEST_SEAMS)
void fail_next_atomic_write_for_test(const std::string& path) {
    detail::g_atomic_write_failure_path = path;
}
#endif

// 目标必须不存在（O_EXCL）；失败时清理目标，供调用方重试。
bool copy_file(const std::string& from, const std::string& to) {
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) return false;
    struct stat st{};
    if (::fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(in);
        return false;
    }
    const int out = ::open(to.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (out < 0) {
        ::close(in);
        return false;
    }
    uint8_t buf[8192];
    bool ok = true;
    for (;;) {
        const ssize_t n = ::read(in, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (n == 0) break;
        size_t offset = 0;
        while (offset < static_cast<size_t>(n)) {
            const ssize_t written = ::write(out, buf + offset, static_cast<size_t>(n) - offset);
            if (written < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            if (written == 0) {
                ok = false;
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (!ok) break;
    }
    if (::close(in) != 0) ok = false;
    if (::close(out) != 0) ok = false;
    if (!ok) {
        ::unlink(to.c_str());
        return false;
    }
    return true;
}

// 回滚还原：先复制到同目录临时文件，再 rename 覆盖目标（目标已存在时 copy_file 不能直写）。
bool copy_file_replacing(const std::string& from, const std::string& to) {
    const std::string tmp = to + ".restore-" + next_file_suffix();
    if (!copy_file(from, tmp)) return false;
    if (::rename(tmp.c_str(), to.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool remove_file(const std::string& path) {
    if (path.empty()) return true;
    return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

std::string file_name_of(const std::string& path) {
    const size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool ends_with(const std::string& value, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return value.size() >= n && value.compare(value.size() - n, n, suffix) == 0;
}

bool is_save_dat_name(const std::string& n) {
    if (n.size() < 8) return false;
    if (n.compare(0, 4, "save") != 0) return false;
    if (n.compare(n.size() - 4, 4, ".dat") != 0) return false;
    for (size_t i = 4; i + 4 < n.size(); ++i) {
        if (n[i] < '0' || n[i] > '9') return false;
    }
    return true;
}

bool dir_contains_save_dat(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return false;
    bool found = false;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        struct stat st{};
        const std::string full = dir + "/" + name;
        if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (is_save_dat_name(name)) {
            found = true;
            break;
        }
    }
    ::closedir(d);
    return found;
}

bool slot_file_exists(const std::string& data_dir, int slot) {
    if (slot < 0 || slot > 2 || data_dir.empty()) return false;
    const std::string name = "save" + std::to_string(slot) + ".dat";
    DIR* d = ::opendir(data_dir.c_str());
    if (d == nullptr) return false;
    bool found = false;
    while (dirent* ent = ::readdir(d)) {
        const std::string entry = ent->d_name;
        if (entry == "." || entry == "..") continue;
        const std::string sub = data_dir + "/" + entry;
        struct stat st{};
        if (::stat(sub.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (file_exists(sub + "/" + name)) {
            found = true;
            break;
        }
    }
    ::closedir(d);
    return found;
}

// 定位原版存档目录：优先 dataDir 下含 save*.dat 的子目录；否则 dataDir/hex(MD5(key)) 并建目录。
bool locate_save_directory(const std::string& data_dir, const std::string& hub_key,
                           std::string& out) {
    if (!data_dir.empty()) {
        DIR* d = ::opendir(data_dir.c_str());
        if (d != nullptr) {
            while (dirent* ent = ::readdir(d)) {
                const std::string name = ent->d_name;
                if (name == "." || name == "..") continue;
                const std::string sub = data_dir + "/" + name;
                struct stat st{};
                if (::stat(sub.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                if (dir_contains_save_dat(sub)) {
                    ::closedir(d);
                    out = sub;
                    return true;
                }
            }
            ::closedir(d);
        }
    }
    if (hub_key.empty()) return false;
    const std::string dir_name = save_backup::md5_hex(
        reinterpret_cast<const uint8_t*>(hub_key.c_str()), hub_key.size());
    const std::string dir = data_dir + "/" + dir_name;
    if (!ensure_dir(dir)) return false;
    out = dir;
    return true;
}

bool backup_dir(const std::string& external_dir, std::string& out) {
    if (external_dir.empty()) return false;
    out = external_dir + "/" + kBackupDirName;
    return ensure_dir(out);
}

bool is_bundle_file_name(const std::string& name) {
    return ends_with(name, kBundleSuffix) || ends_with(name, kLegacyBundleSuffix);
}

bool list_bundle_files(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return false;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        const std::string full = dir + "/" + name;
        struct stat st{};
        if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (is_bundle_file_name(name)) out.push_back(name);
    }
    ::closedir(d);
    return true;
}

std::vector<BackupFile> list_backup_files(const std::string& dir) {
    std::vector<BackupFile> files;
    std::vector<std::string> names;
    if (!list_bundle_files(dir, names)) return files;
    for (const std::string& name : names) {
        const std::string path = dir + "/" + name;
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) continue;
        files.push_back(BackupFile{
            name, static_cast<uint64_t>(st.st_size),
            static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 +
                static_cast<int64_t>(st.st_mtim.tv_nsec) / 1000000});
    }
    return files;
}

std::string find_by_checksum(const std::string& dir, const std::string& checksum) {
    std::vector<std::string> names;
    if (!list_bundle_files(dir, names)) return std::string();
    for (const std::string& name : names) {
        const std::string full = dir + "/" + name;
        std::vector<uint8_t> bytes;
        save_backup::ParsedBundle parsed;
        if (!read_file_bytes(full, bytes) ||
            !save_backup::parse_bundle(bytes.data(), bytes.size(), parsed)) {
            continue;
        }
        if (save_backup::bundle_matches_checksum(name, parsed, checksum)) return full;
    }
    return std::string();
}

std::optional<std::string> auto_backup_path(const std::string& external_dir, int slot) {
    if (external_dir.empty() || slot < 0 || slot > 2) return std::nullopt;
    const std::string base =
        external_dir + "/backups/autobackup/autosave_s" + std::to_string(slot);
    const std::string path = base + kBundleSuffix;
    if (file_exists(path)) return path;
    const std::string legacy = base + kLegacyBundleSuffix;
    if (file_exists(legacy)) return legacy;
    return std::nullopt;
}

long long now_ms() {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string next_transaction_id() {
    return std::to_string(g_file_seq.fetch_add(1, std::memory_order_relaxed) + 1);
}

bool local_stamp(long long ms, char out[16]) {
    const time_t sec = static_cast<time_t>(ms / 1000);
    tm tmv{};
    if (::localtime_r(&sec, &tmv) == nullptr) return false;
    return ::strftime(out, 16, "%Y%m%d-%H%M%S", &tmv) == 15;
}

std::mutex& file_mutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace persistence
}  // namespace save_backup
