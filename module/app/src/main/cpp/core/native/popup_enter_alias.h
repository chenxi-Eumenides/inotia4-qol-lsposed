#pragma once

#include <cstdint>

// 模块把 PopupState 的 enter 回调替换成自己的 wrapper 后，popup 栈顶保存的是模块
// .so 内的地址；UI 判定层用「enter - g_base」（g_base = libgame.so 基址）换算会得到
// 负值，被当作「无面板」而误报 world。各功能域安装 wrapper 时登记
// (wrapper, 原版 enter VMA)，判定层据此把栈顶还原成原版面板 VMA。
// 固定槽位、无分配：登记发生在功能安装期（游戏线程），查询来自 API 线程。

// 未登记时的返回值（也等价于「无原版 VMA」，调用方保持原有换算路径）。
constexpr uintptr_t kPopupEnterAliasNone = 0;

// 登记 wrapper 对应的原版 enter VMA；wrapper/vma 为 0 时忽略。
// 同一 wrapper 重复登记覆盖其 VMA；槽位耗尽时登记失败（仅影响 UI 态显示）。
void popup_enter_alias_register(uintptr_t wrapper, uintptr_t original_vma);

// 注销 wrapper；未登记返回 false。
bool popup_enter_alias_unregister(uintptr_t wrapper);

// 按栈顶 enter 反查原版 enter VMA；未登记返回 kPopupEnterAliasNone。
uintptr_t popup_enter_alias_resolve(uintptr_t enter);

// 清空全部登记（生命周期复位与 host 测试用）。
void popup_enter_alias_clear();
