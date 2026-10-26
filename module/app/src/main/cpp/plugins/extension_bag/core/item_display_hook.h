#pragma once

#include "contracts/ui.h"

namespace qol::features::extension_bag {

// ControlItem_SetItem / ControlItem_GetItem 的投影替换入口。
// 只在当前装备页、商店或合成投影窗口内替换 item；其余控件与非投影路径走原函数。
void extension_bag_control_item_set_item(void* control, void* item);
void* extension_bag_control_item_get_item(void* control);
int extension_bag_inven_get_bag_size(int bag);

// 安装期 pending 上下文：refresh 前 begin，提交时 commit，失败时 cancel。
void extension_bag_display_begin_pending(qol::contracts::QolUiRootKind kind, int bag,
                                         int window_bag);
void extension_bag_display_commit();
void extension_bag_display_cancel();

// 只有 EB 自己的显示刷新/绘制窗口才把 INVEN_GetBagSize 投影成扩展容量。
// core.item 等其他调用方在窗口外读取时必须拿到真实原版容量。
class BagSizeProjectionScope {
public:
    BagSizeProjectionScope();
    ~BagSizeProjectionScope();
    BagSizeProjectionScope(const BagSizeProjectionScope&) = delete;
    BagSizeProjectionScope& operator=(const BagSizeProjectionScope&) = delete;

private:
    bool entered_ = false;
};

// 恢复/回滚 refresh 期间抑制替换，让原版控件重新读到真实槽表。
class DisplaySubstitutionSuppressScope {
public:
    DisplaySubstitutionSuppressScope();
    ~DisplaySubstitutionSuppressScope();
    DisplaySubstitutionSuppressScope(const DisplaySubstitutionSuppressScope&) = delete;
    DisplaySubstitutionSuppressScope& operator=(const DisplaySubstitutionSuppressScope&) = delete;

private:
    bool previous_ = false;
};

}  // namespace qol::features::extension_bag
