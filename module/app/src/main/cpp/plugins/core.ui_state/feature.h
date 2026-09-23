#pragma once

#include <cstddef>
#include <cstdint>

#include "../../contracts/plugin.h"
#include "../../contracts/ui_choice.h"

// 原生选择框插件业务层（原 feature/ui/native_choice，D1-b1 迁移）。
//
// 资源由宿主声明式安装（plugin.cpp）：
//   · hook:UIChoice_Init（kEntryHook）——标题改写 wrapper，backup 经 on_resource_ready 交还；
//   · ptrslot:g_uichoice_button_list_exe_got（kPtrSlot）——按钮 ExecuteProc 槽，wrapper 截获选择。
// 本层不直接安装机制、不出现裸地址/符号头。
//
// 纯逻辑规则（native_choice_detail）同时供实现与 host 测试使用，不触碰游戏内存。

namespace qol::features::core_ui_state {

namespace native_choice_detail {

constexpr int kMaxItems = 6;

inline bool valid_spec(const contracts::UiChoiceSpec& spec) {
    if (spec.items == nullptr || spec.count < 1 || spec.count > kMaxItems) return false;
    if (spec.close_index < -1 || spec.close_index >= spec.count) return false;
    for (int i = 0; i < spec.count; ++i) {
        if (spec.items[i] == nullptr) return false;
    }
    return true;
}

inline bool can_open(bool active, const contracts::UiChoiceSpec& spec) {
    return !active && valid_spec(spec);
}

inline bool is_close_index(int index, int close_index) {
    return close_index >= 0 && index == close_index;
}

inline bool address_in_range(uintptr_t address, uintptr_t begin, size_t size) {
    return size != 0 && address >= begin && address - begin < size;
}

}  // namespace native_choice_detail

// 能力接口实现（签名与迁移前公开函数逐字一致）。
bool native_choice_open(const contracts::UiChoiceSpec& spec);
bool native_choice_active();
void native_choice_close();

// 资源 handler（plugin.cpp 的 ResourceClaim 取址）：
void choice_init_wrapper(void* control);    // hook:UIChoice_Init
void choice_button_execute(void* control);  // ptrslot:...button_list_exe_got

// backup 交还（§3.3）：UIChoice_Init 原函数 / 按钮槽原 ExecuteProc。
int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup);

// 启用/停用标记同步（宿主 lifecycle 回调调用）：open 的门禁。
void native_choice_set_installed(bool installed);

}  // namespace qol::features::core_ui_state
