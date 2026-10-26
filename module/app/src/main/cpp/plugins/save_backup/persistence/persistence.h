#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "plugins/save_backup/model/types.h"

// save_backup L2：文件原语、目录定位与 bundle 枚举。
// 本层不做游戏调用、不拼 JSON、不持有业务状态；目录/密钥由调用方传入或取 state 快照。

namespace save_backup {
namespace persistence {

inline constexpr const char* kBackupDirName = "backups";
inline constexpr const char* kBundleSuffix = ".qol.bak";
// 旧版导出的后缀：继续列出/解析/导入，避免升级后旧备份消失。
inline constexpr const char* kLegacyBundleSuffix = ".qol_save";

bool ensure_dir(const std::string& path);
bool file_exists(const std::string& path);
bool read_file_bytes(const std::string& path, std::vector<uint8_t>& out);
bool write_file_bytes_atomic(const std::string& path, const uint8_t* data, size_t size);
bool copy_file(const std::string& from, const std::string& to);
bool copy_file_replacing(const std::string& from, const std::string& to);
bool remove_file(const std::string& path);
std::string file_name_of(const std::string& path);
bool ends_with(const std::string& value, const char* suffix);

// save{slot}.dat / save*.dat 判定与目录扫描。
bool is_save_dat_name(const std::string& name);
bool dir_contains_save_dat(const std::string& dir);
bool slot_file_exists(const std::string& data_dir, int slot);
// 优先 data_dir 下含 save*.dat 的子目录；否则 data_dir/hex(MD5(hub_key))（会建目录）。
// 返回 true 时 out 为存档目录。
bool locate_save_directory(const std::string& data_dir, const std::string& hub_key,
                           std::string& out);

// 备份目录与 bundle 枚举（新 .qol.bak + 旧 .qol_save）。
bool backup_dir(const std::string& external_dir, std::string& out);
bool is_bundle_file_name(const std::string& name);
bool list_bundle_files(const std::string& dir, std::vector<std::string>& out);
std::vector<BackupFile> list_backup_files(const std::string& dir);
std::string find_by_checksum(const std::string& dir, const std::string& checksum);
// 自动备份：autosave_sN.qol.bak 优先，旧 autosave_sN.qol_save 回退；不存在返回空 optional。
std::optional<std::string> auto_backup_path(const std::string& external_dir, int slot);

long long now_ms();
std::string next_transaction_id();
bool local_stamp(long long ms, char out[16]);

// 窄文件锁：bundle 读改写（backfill/删除/导入定位）串行；不承载业务状态。
std::mutex& file_mutex();

}  // namespace persistence
}  // namespace save_backup
