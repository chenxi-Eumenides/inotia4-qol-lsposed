#pragma once

#include <cstdint>

#include "contracts/panels.h"

// 面板 provider 声明表（ARCHITECTURE.md §3.4）：纯数据面，零 game 依赖。
//
// D 方案：provider 由 PluginDescriptor 在装配期声明，本表只追加、永不重排/删除/复用槽位；
// 插件启停只翻 available 位。持有表内地址的调用方（PopupHost 栈、模块面板栈）因此
// 在整个进程生命周期内都能读到原来的声明项。
//
// 设置页条目表（PanelItemSpec）已无产品调用方，D 改造中删除内部实现；
// PanelItemSpec 类型与 Host SDK register_item 槽位仅为 ABI 前缀保留。
//
// 线程纪律：声明只在装配期发生；available 只由宿主主线程更新；本模块不引入锁。

namespace qol::infra {

// 装配期校验一组声明（不改表）：owner 非空、数量不超单插件上限、provider 字段完整、
// owner 与 panel_id 组合在全表内唯一。返回 nullptr 表示可以登记，否则返回静态原因串。
const char* panels_validate_declaration(const char* owner,
                                        const contracts::PanelProviderSpec* providers,
                                        uint32_t count);

// 装配期追加一条声明（available=false）。校验失败或容量耗尽返回 false。
bool panels_declare_provider(const contracts::PanelProviderSpec& provider);

// 只翻可用位，不增删/移动条目。owner 空 → 无操作。
void panels_set_owner_available(const char* owner, bool available);

// 可用项数/按声明顺序取可用项；越界返回 nullptr。
uint32_t panels_provider_count();
const contracts::PanelProviderSpec* panels_provider_at(uint32_t index);

// 按 panel_id 查找**可用** provider；未声明或不可用返回 nullptr。
const contracts::PanelProviderSpec* panels_find_provider(const char* panel_id);

// 测试用：清空声明表。
void panels_reset_for_test();

}  // namespace qol::infra
