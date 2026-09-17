#pragma once

#include <string>

// P1 最小诊断接口的数据源：把游戏变体（libgame.so 查表结果）导出为 JSON。
// 命名空间取 qol::host：该文件属「宿主装配层」，位于 bridge 与 core/feature 之间，
// 供 bridge/native 的 JNI 薄层调用，不反向依赖 JNI。

namespace qol::host {

// 返回 {"series":"...","version":"...","known":<bool>,"capabilities":<int>}。
// 本函数为 thin formatter：只读 core/native/game_variant.h 的查表结果，不含分支决策。
// 因无注入点，不参与 host 单测（无法在 host 侧伪造变体表），验证走真机 curl。
std::string host_variant_json();

}  // namespace qol::host
