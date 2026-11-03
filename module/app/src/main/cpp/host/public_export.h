#pragma once

#include "contracts/public_export.h"

// Host 侧的公共 Download 导出门面。API 对象由 Host 装配进 HostSdk 尾字段；
// 实际 JNI 调用在 bridge/native/gamebridge_public_export.cpp，本层只做错误语义转换。
namespace qol::host {

const contracts::QolPublicExportApi& public_export_api();

// bridge 层实现：Kotlin bridge 未注册或调用失败时返回 false（由调用方转成 QolResult）。
bool public_export_call_crash_logs();
bool public_export_call_backup(const char* source_path, const char* display_name);
bool public_export_call_pick_file();

}  // namespace qol::host
