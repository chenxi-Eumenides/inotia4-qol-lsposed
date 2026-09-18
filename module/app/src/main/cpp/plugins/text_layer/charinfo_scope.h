#pragma once

// 角色属性面板（POPUP_SC_CHARACTER_INFO）战斗属性行标签中文化：绘制窗口作用域标记。
//
// 文案表本身在 text_layer 插件内（feature.h 的 `charinfo.*` 条目，kCharacterPanel 作用域）
// ——`MEMORYTEXT_GetText` 是全游戏唯一文本热点且 **LSPosed NativeHook 对同一地址二次挂载
// 会失败**，故该 hook 由 text_layer 独占；本功能不自己挂 GetText。
//
// 本文件只剩一件事：把角色面板的绘制窗口标记为 `kCharacterPanel` 作用域。text_layer 的
// GetText wrapper 只在当前线程处于该作用域时才替换 35185..35196 的返回值，其它界面一律原样。
//
// 设计依据：ARCHITECTURE.md §3.3（插件清单）/§4.3（资源描述符）。

#include <cstdint>

namespace qol::features::text_layer {

// 资源 handler：角色面板每帧绘制入口 wrapper（kEntryHook，owner=text_layer）。
void scene_draw_charinfo_wrapper();

// on_resource_ready 交还 backup（原函数入口）：缓存供 wrapper 转发原调用。
// 无条件接受（与 memorytext_get_text 的 backup 交付同规则）；wrapper 侧自行判空。
int32_t charinfo_scope_on_resource_ready(void* backup);

}  // namespace qol::features::text_layer
