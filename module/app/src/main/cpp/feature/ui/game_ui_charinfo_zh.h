#pragma once

// 角色属性面板（POPUP_SC_CHARACTER_INFO）战斗属性行标签中文化。
//
// 文案表本身已迁到统一设施 `feature/ui/module_text`（`Scope::kCharacterPanel` 的 `charinfo.*`
// 条目）——`MEMORYTEXT_GetText` 是全游戏唯一文本热点且 **LSPosed NativeHook 对同一地址二次挂载
// 会失败**，故该 hook 由 module_text 独占；本功能不再自己挂 GetText。
//
// 本文件只剩一件事：把角色面板的绘制窗口标记为 `kCharacterPanel` 作用域。module_text 的
// GetText wrapper 只在当前线程处于该作用域时才替换 35185..35196 的返回值，其它界面一律原样。
//
// 设计依据：docs/development/features/custom-text.md（模块自定义文本层）、
// docs/reference/game/ui.md §5（文本表）、architecture.md §2.2.1（Native Hook 选型）。

// 安装角色面板绘制窗口 hook（幂等；bridge 未就绪时静默返回，可后续重试）。
// 调用时机：nativeInit 在 bridge_init 成功后，且需在 module_text_install_if_ready() 之后。
void charinfo_zh_install_if_ready();
