#pragma once

#include "contracts/game_ports/system_port.h"

namespace qol::game {

const contracts::QolSystemPort& system_port();

// 启动期由 Kotlin 下发模块身份（BuildConfig.VERSION_NAME / TargetPackages.current()）；
// nullptr 视为空。运行期只读，仅 system_facts 的 /api/system/info 与 /game 使用。
// 身份未下发前 port 不产出 info/game body，避免响应与冻结契约不符。
void system_port_set_identity(const char* version, const char* package);

// 模块身份版本（同上版本字段）；未下发返回 nullptr。返回静态存储，进程内有效，
// 调用方仅可读取，不得写入或释放。
const char* system_port_version();

// 目标包名（同上版本字段）；未下发返回 nullptr。返回静态存储，进程内有效，
// 调用方仅可读取，不得写入或释放。
const char* system_port_package();

}  // namespace qol::game
