// Host 测试用的 public export bridge 桩：JNI 实现在 bridge/native/，不参与 host 测试链接。
#include "host/public_export.h"
#include "host/plugin_registry.h"

namespace qol::host {

bool public_export_call_crash_logs() { return false; }
bool public_export_call_backup(const char*, const char*) { return false; }
bool public_export_call_pick_file() { return false; }

// 编译了 host/features/reconciler.cpp 但没有链接 host/plugin_registry.cpp 的 target 使用这两个桩。
// 这些 target 不跑 registry 生命周期语义，故始终返回「无失败 / 无瞬态标记」。
int32_t feature_registry_last_failure_code(const char*) { return 0; }
bool feature_registry_pop_transient_failure(const char*) { return false; }

}  // namespace qol::host
