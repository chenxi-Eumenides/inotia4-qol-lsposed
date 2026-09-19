#pragma once

#include "../../contracts/plugin.h"

// 合成器界面层插件清单入口（ARCHITECTURE.md §3.3；C2 Step 3）。
//
// 12 条 UIMix 入口 hook（5 核心 required/fail-closed + 7 装饰 optional/fail-soft，其中
// 「清空填入格」常驻无门禁）由宿主声明式安装；wrapper 只做界面判断/呈现，配方判定与扣料
// 决策经能力契约 `craft_result`（optional_requires）问 provider=plugins/custom_recipe。
// 消费方式：每次调用现取接口、不缓存跨帧指针（provider 撤销期降级为转调原版/无操作）。

namespace qol::features::craft_ui {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::craft_ui
