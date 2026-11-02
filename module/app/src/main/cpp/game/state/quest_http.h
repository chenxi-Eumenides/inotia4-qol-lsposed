#pragma once

#include <cstdint>
#include <string>

namespace qol::game {

// quest owner 的写路由结果拼装接缝（实现在 legacy 聚合 TU game_system.cpp，该文件可访问
// api/native 数据源；game 层适配端口只依赖本声明）。
//
// 必须在游戏主线程调用（由 quest 插件的结果处理器经 QolQuestPort.write.quit_result 触发）。
// 复刻迁移前 Kotlin ActionSupport.attachPlayer(NativeBridge.nativeOpQuestQuit(id)) 的结果：
// 成功 → {"ok":true,"state":<player JSON>}；失败 → 原错误体原样。
std::string build_quest_quit_result_json(int32_t quest_id);

}  // namespace qol::game
