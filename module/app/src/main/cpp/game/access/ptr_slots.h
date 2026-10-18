#pragma once

#include <cstdint>

// 游戏层：指针槽站点登记（ARCHITECTURE.md §4.1 指针槽、§4.3 资源描述符）。
// 与 game/access/patches.h 的 patch_site 对称：把「登记名 → 运行期槽地址 + 安装前
// 期望值」交给 patch 层，使指针槽机制的输入不含裸地址，游戏侧 VMA/偏移保持
// game_symbols.h 单一来源。机制实现不得在本层出现。

namespace qol::game {

struct PtrSlotSite {
    uintptr_t slot_addr = 0;       // 运行期函数指针槽地址（如 GOT 条目）
    void* expected_value = nullptr;  // 安装前该槽应存的原函数入口
};

struct DescButtonTarget {
    void* button = nullptr;
    void* data = nullptr;
    void* execute_slot = nullptr;
    void* original_proc = nullptr;
    uintptr_t panel_slot_addr = 0;
};

// 按登记名解析指针槽站点；未登记 / 基址未就绪 / 符号未解析 / 槽不可读 → false。
bool ptr_slot_site(const char* name, PtrSlotSite* out);

// 动态装备详情按钮执行槽；索引对应 UIEquip 面板的 11 个描述按钮槽。
// 槽地址和安装前函数入口只在宿主内部返回，插件通过 Host SDK 的类型化绑定面使用。
bool extension_desc_button_slot(uint32_t index, PtrSlotSite* out);
bool extension_desc_button_instance(uint32_t index, void** out_button);
bool extension_desc_button_target(uint32_t index, DescButtonTarget* out);
bool extension_desc_button_identity(void* button, void** out_data,
                                    void** out_execute_slot, void** out_current_proc);
bool extension_desc_button_original_matches(void* original, const char* symbol);

}  // namespace qol::game
