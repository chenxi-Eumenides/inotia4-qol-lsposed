#pragma once

#include <string>

namespace qol::game {

// system_facts owner 的响应体拼装接缝（实现在 legacy/system 的 game_system.cpp，
// 该文件可访问 infra/legacy 数据源；game 层适配端口只依赖本声明）。
//
// 这些函数必须在游戏主线程调用，由 system_facts 帧任务发布为快照；HTTP handler 不直接调用。

// GET /api/system/snapshot（原生快照 + 迁移前 Kotlin 名称注入/字段重排）。
std::string build_system_snapshot_json();

// GET /api/system/game：原始快照 + 调用方传入的 info 体（与迁移前 Kotlin 拼装逐字节一致）。
std::string build_system_game_json(const std::string& info_json);

// GET /api/system/info：version/package 由启动期下发的模块身份传入。
std::string build_system_info_json(const std::string& version, const std::string& package);

// GET /api/system/game_frame。
std::string build_system_frame_json();

}  // namespace qol::game
