#pragma once

#include <cstdint>

#include "result.h"

// 把模块 ExternalDir 下已有的文件复制到公共 Download 目录（MediaStore.Downloads）。
// 由 Host 提供，异步执行：调用只入队、立即返回；完成结果由 Kotlin 侧写日志并提示用户。
// 源路径必须位于模块 ExternalDir（qol/）内，越界路径按 kQolErrInvalidArgument 拒绝。
namespace qol::contracts {

constexpr int32_t kPublicExportVersionMajor = 1;
constexpr int32_t kPublicExportVersionMinor = 0;

struct QolPublicExportApi {
    uint32_t struct_size;
    // 把 qol/qol-crash.log.prev、qol/java-crash.log.prev 复制到 Download/qol/。
    QolResult (*export_crash_logs)();
    // 把 source_path 指向的文件复制到 Download/qol/backups/，目标文件名用 display_name。
    QolResult (*export_backup)(const char* source_path, const char* display_name);
    // 拉起系统文件选择器（SAF）；选中文件由 Kotlin 归入 qol/backups/ 后刷新存档面板。
    // 返回 kQolErrBlocked 表示当前没有可用的宿主 Activity。
    QolResult (*pick_import_file)();
};

}  // namespace qol::contracts
