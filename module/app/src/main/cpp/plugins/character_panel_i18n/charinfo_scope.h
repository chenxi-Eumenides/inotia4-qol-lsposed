#pragma once

// 角色属性面板（POPUP_SC_CHARACTER_INFO）绘制窗口作用域标记。
//
// 文案表由本插件持有，core.custom_ui 经 text_layer 能力提供文本机制。`MEMORYTEXT_GetText`
// 由 core.custom_ui 持有唯一 hook，避免 LSPosed NativeHook 对同一地址重复挂载。
//
// 本文件把角色面板绘制窗口标记为 kCharacterPanel 作用域；文本机制仅在该作用域内替换标签。

#include <cstdint>

namespace qol::features::character_panel_i18n {

// 资源 handler：角色面板每帧绘制入口 wrapper（kEntryHook，owner=character_panel_i18n）。
void scene_draw_charinfo_wrapper();

// on_resource_ready 交还非空 backup（原函数入口）：缓存供 wrapper 转发原调用。
int32_t charinfo_scope_on_resource_ready(void* backup);

}  // namespace qol::features::character_panel_i18n
