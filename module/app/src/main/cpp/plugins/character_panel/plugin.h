#pragma once

#include "../../contracts/plugin.h"

// character_panel 插件（ADR 0001 D4）：把原版人物属性面板改造成属性总览入口。
// 声明面见：
//   feature.h          —— 12 条汉化标签表与 text_layer 消费者注册；
//   charinfo_scope.h   —— 原版面板绘制窗口的 kCharacterPanel 作用域 hook；
//   charinfo_watch.h   —— 原版面板 PopupState 占用、点击检测与延迟打开请求；
//   overview.h         —— 自定义属性总览面板 provider（最小骨架）与开关门禁。

namespace qol::features::character_panel {

const contracts::PluginDescriptor& plugin_descriptor();

}  // namespace qol::features::character_panel
