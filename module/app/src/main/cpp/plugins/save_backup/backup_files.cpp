#include "backup_files.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>

#include "log.h"

namespace qol::features::save_backup {
namespace {

#define SB_LOG(...) SAVE_BACKUP_LOG_INFO(__VA_ARGS__)

BackupFilesSource g_source{};

bool source_ready() {
    return g_source.read_original_dat != nullptr && g_source.read_module_sidecar != nullptr &&
           g_source.locate_save_directory != nullptr && g_source.read_file_bytes != nullptr;
}

bool ends_with(const std::string& value, const char* suffix) {
    const size_t size = std::strlen(suffix);
    return value.size() >= size && value.compare(value.size() - size, size, suffix) == 0;
}

// 与导出旧实现一致：收集 save{slot}.dat.wh4-*，失败即整次导出失败。
bool collect_warehouse(int slot, SlotSnapshot& out, std::string& err) {
    std::string save_dir;
    if (!g_source.locate_save_directory(save_dir)) return true;  // 目录不可得 = 无外部仓库

    const std::string dat_name = "save" + std::to_string(slot) + ".dat";
    const std::vector<std::string> names = list_warehouse_companions(save_dir, slot);
    for (const std::string& name : names) {
        const std::string suffix = name.substr(dat_name.size());
        // 非合法仓库伴生名（如 `.wh4-` 后带路径字符）不是仓库文件，跳过。
        if (!::save_backup::valid_warehouse_suffix(suffix)) {
            SB_LOG("save backup export slot=%d: skip invalid warehouse name: %s", slot,
                   name.c_str());
            continue;
        }
        const std::string full = save_dir + "/" + name;
        struct stat info{};
        if (::stat(full.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 &&
            static_cast<uint64_t>(info.st_size) > ::save_backup::kMaxWarehouseDataBytes) {
            SB_LOG("save backup export slot=%d: warehouse too large (%lld): %s", slot,
                   static_cast<long long>(info.st_size), name.c_str());
            err = "warehouse file too large";
            return false;
        }
        std::vector<uint8_t> bytes;
        if (!g_source.read_file_bytes(full, bytes)) {
            SB_LOG("save backup export slot=%d: warehouse read failed: %s", slot, name.c_str());
            err = "warehouse read failed";
            return false;
        }
        if (bytes.size() > ::save_backup::kMaxWarehouseDataBytes) {
            SB_LOG("save backup export slot=%d: warehouse too large (%zu): %s", slot, bytes.size(),
                   name.c_str());
            err = "warehouse file too large";
            return false;
        }
        ::save_backup::WarehouseFile file;
        file.suffix = suffix;
        // 优先解密为明文（跨设备可重加密）；失败则保留密文，导入走旧校验路径。
        bool decrypted = false;
        if (bytes.size() >= 4 && g_source.encrypt_available != nullptr &&
            g_source.encrypt_available()) {
            const char* key = g_source.save_hub_key != nullptr ? g_source.save_hub_key() : nullptr;
            if (key != nullptr && g_source.encrypt_process != nullptr) {
                const size_t payload_len = bytes.size() - 3;
                std::vector<uint8_t> buffer = bytes;
                if (g_source.encrypt_process(buffer.data(), static_cast<int32_t>(payload_len), 1,
                                             key) == 1) {
                    file.data.assign(buffer.begin(), buffer.begin() + payload_len);
                    file.flags = ::save_backup::kWarehouseFlagPlain;
                    decrypted = true;
                    SB_LOG("save backup export slot=%d: warehouse decrypted for cross-device: %s "
                           "(%zu bytes)",
                           slot, name.c_str(), file.data.size());
                }
            }
        }
        if (!decrypted) {
            file.data.swap(bytes);
            file.flags = 0;
            SB_LOG("save backup export slot=%d: warehouse kept as ciphertext (decrypt "
                   "unavailable): %s (%zu bytes)",
                   slot, name.c_str(), file.data.size());
        }
        out.warehouse.files.push_back(std::move(file));
    }

    // 容量校验早于组装；错误串与旧导出路径一致。
    std::string warehouse_error;
    if (!::save_backup::validate_warehouse(out.warehouse.files, warehouse_error)) {
        SB_LOG("save backup export slot=%d: warehouse validation failed: %s", slot,
               warehouse_error.c_str());
        err = warehouse_error;
        return false;
    }
    // 有外部 wh4 时剥离存档 block3 内嵌仓库段；无外部 wh4 时不得剥离（否则丢仓库）。
    if (!out.warehouse.files.empty()) {
        const ::save_backup::InlineWarehouseStrip strip =
            ::save_backup::warehouse_strip_inline_segment(out.dat.plain);
        if (strip == ::save_backup::InlineWarehouseStrip::kStripped) {
            SB_LOG("save backup export slot=%d: inline WH96v002 segment stripped (origLen=%zu)",
                   slot, out.dat.plain.size());
        } else {
            SB_LOG("save backup export slot=%d: inline warehouse kept (%s)", slot,
                   ::save_backup::inline_warehouse_strip_name(strip));
        }
    } else {
        SB_LOG("save backup export slot=%d: no external warehouse collected, keep inline "
               "warehouse",
               slot);
    }
    return true;
}

}  // namespace

const BackupFileClass* backup_file_classes(size_t* count) {
    // checksum 顺序固定为 dat → module → warehouse；三类都参与 v3 全内容校验。
    static const BackupFileClass kClasses[] = {
        {BackupFileKind::kOriginalDat, "original_dat", nullptr, true, true},
        {BackupFileKind::kModuleSidecar, "module_sidecar", "module_state", true, true},
        {BackupFileKind::kWarehouseCompanion, "warehouse_companion", nullptr, true, true},
    };
    if (count != nullptr) *count = sizeof(kClasses) / sizeof(kClasses[0]);
    return kClasses;
}

std::vector<std::string> list_warehouse_companions(const std::string& save_dir, int slot) {
    const std::string dat_name = "save" + std::to_string(slot) + ".dat";
    const std::string prefix = dat_name + ".wh4-";
    DIR* dir = ::opendir(save_dir.c_str());
    if (dir == nullptr) return {};
    std::vector<std::string> names;
    while (dirent* ent = ::readdir(dir)) {
        const std::string name = ent->d_name;
        if (name.size() <= prefix.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (ends_with(name, ".tmp") || ends_with(name, ".wh-tmp")) continue;
        const std::string full = save_dir + "/" + name;
        struct stat info{};
        if (::stat(full.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
        if (!::save_backup::valid_warehouse_suffix(name.substr(dat_name.size()))) continue;
        names.push_back(name);
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

void backup_files_bind_source(const BackupFilesSource& source) { g_source = source; }

bool collect_slot_snapshot(int slot, SlotSnapshot& out, std::string& err) {
    err.clear();
    out = SlotSnapshot{};
    if (slot < 0 || slot > 2) {
        err = "bad slot";
        return false;
    }
    if (!source_ready()) {
        err = "backup files unavailable";
        return false;
    }
    if (!g_source.read_original_dat(slot, out.dat, err)) return false;
    if (!g_source.read_module_sidecar(slot, out.module, err)) return false;
    return collect_warehouse(slot, out, err);
}

}  // namespace qol::features::save_backup
