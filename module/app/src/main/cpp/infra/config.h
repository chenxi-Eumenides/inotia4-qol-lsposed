#pragma once

// 运行时配置（ARCHITECTURE.md §3.5 配置、§6.5）：键值经事件广播变更。
// 固定容量 64 键；键字符串按值拷贝进静态缓冲（63 字符 + NUL）；int32 值；不使用 STL 容器。
//
// 值的变更通过 infra/events 的 kEventConfigChanged 广播；publish 在调用线程同步执行
// （与 events.h 一致）。P2 仅内存态，外部 config.json 持久化属后续阶段。

#include <cstdint>

namespace qol::infra {

// 值有变化才广播；key 为空、过长或容量满返回 false。
bool config_set_int(const char* key, int32_t value);

// String values use fixed inline storage because the config registry does not retain caller pointers.
bool config_set_string(const char* key, const char* value);
const char* config_get_string(const char* key, const char* default_value);

// 未登记返回 default_value。
int32_t config_get_int(const char* key, int32_t default_value);

// bool 以 0/1 存储。
bool config_set_bool(const char* key, bool value);
bool config_get_bool(const char* key, bool default_value);

// 测试用：清空全部键值。
void config_reset_for_test();

}  // namespace qol::infra
