#pragma once

#include <string>

namespace qol::game {

// character_facts owner 的响应体拼装接缝（实现在 legacy/system 的 game_system.cpp，
// 该文件可访问 infra/legacy 数据源；game 层适配端口只依赖本声明）。
//
// 这些函数必须在游戏主线程调用，由 character_facts 帧任务发布为快照；HTTP handler 不直接调用。
// 非 world 状态返回 `{"error":"not in game"}`（与迁移前 Kotlin 收到的原生体一致）。

// 原生 party 数组 + 迁移前 Kotlin NameInjector.withItemNames 的加工（成员名称/职业注入、
// restructureRole 重排、装备逐件 restructureItem）。
std::string build_character_party_json();

// 原生 skills 数组 + 迁移前 Kotlin NameInjector.injectSkillNames 的技能名注入。
std::string build_character_skills_json();

// 原生 mercenaries 数组（原样透传）。
std::string build_character_mercenaries_json();

}  // namespace qol::game
