// Host 测试用的 public export bridge 桩：JNI 实现在 bridge/native/，不参与 host 测试链接。
#include "host/public_export.h"

namespace qol::host {

bool public_export_call_crash_logs() { return false; }
bool public_export_call_backup(const char*, const char*) { return false; }
bool public_export_call_pick_file() { return false; }

}  // namespace qol::host
