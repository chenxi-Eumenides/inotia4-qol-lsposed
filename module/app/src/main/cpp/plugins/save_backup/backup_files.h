#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bundle.h"

namespace qol::features::save_backup {

// 导出侧需要备份的文件类别。删槽参与者先登记，删除行为在后续批次接入。
enum class BackupFileKind : uint8_t {
    kOriginalDat = 0,
    kModuleSidecar = 1,
    kWarehouseCompanion = 2,
};

struct CollectedDat {
    std::vector<uint8_t> plain;  // 已把块0槽字节归一化为 0xFF
    int map_id = 0;
    long long save_time = 0;
    int save_version = 0;
    int hero_level = -1;
    int hero_index = -1;
    int class_idx = -1;
};

struct CollectedSidecar {
    enum class State : uint8_t { kAbsent = 0, kPresent = 1 };
    State state = State::kAbsent;
    std::vector<uint8_t> bytes;
};

struct CollectedWarehouse {
    std::vector<::save_backup::WarehouseFile> files;
};

struct SlotSnapshot {
    CollectedDat dat;
    CollectedSidecar module;
    CollectedWarehouse warehouse;
};

// 每类文件的登记项：id、meta 状态字段、是否参与 checksum、是否参与删槽（后续批次消费）。
struct BackupFileClass {
    BackupFileKind kind;
    const char* id;
    const char* meta_state_field;
    bool checksum_participates;
    bool delete_participates;
};

const BackupFileClass* backup_file_classes(size_t* count);

// 收集一个槽的全部可备份文件；失败时 err 为现有导出错误串。
bool collect_slot_snapshot(int slot, SlotSnapshot& out, std::string& err);

// 插件内部桥接：游戏/存储/文件原语由 feature.cpp 提供，保证导出与导入共用同一实现。
struct BackupFilesSource {
    bool (*read_original_dat)(int32_t slot, CollectedDat& out, std::string& err);
    bool (*read_module_sidecar)(int32_t slot, CollectedSidecar& out, std::string& err);
    bool (*locate_save_directory)(std::string& out);
    bool (*read_file_bytes)(const std::string& path, std::vector<uint8_t>& out);
    bool (*encrypt_available)();
    const char* (*save_hub_key)();
    int (*encrypt_process)(void* data, int32_t len, int32_t mode, const char* key);
};
void backup_files_bind_source(const BackupFilesSource& source);

}  // namespace qol::features::save_backup
