#pragma once

#include "contracts/game_ports/system_port.h"

namespace qol::game {

const contracts::QolSystemPort& system_port();

// 启动期由 Kotlin 下发模块身份（BuildConfig.VERSION_NAME / TargetPackages.current()）和宿主游戏版本；
// nullptr 视为空。运行期只读：system_facts 的 info/game 使用模块版本与包名，Game Host 身份面另读取游戏版本名。
void system_port_set_identity(const char* version, const char* package, const char* game_version);

// 模块身份版本（同上版本字段）；未下发返回 nullptr。返回静态存储，进程内有效，
// 调用方仅可读取，不得写入或释放。
const char* system_port_version();

// 宿主游戏 APK versionName；未下发返回 nullptr。返回静态存储，进程内有效，
// 调用方仅可读取，不得写入或释放。
const char* system_port_game_version();

// 目标包名（同上版本字段）；未下发返回 nullptr。返回静态存储，进程内有效，
// 调用方仅可读取，不得写入或释放。
const char* system_port_package();

}  // namespace qol::game
