#pragma once

#include "contracts/ui.h"

namespace qol::features::core_ui {

// 两行上下放置的选项组。每行用帮助页选项的原生背景（标题/选项区域组件），
// 点击任意一行切换 selected；组本身不创建面板，由调用方把 rect 放到目标面板上。
using UiOptionGroupChangeFn = void (*)(void* user, int32_t selected);

struct UiOptionGroupDesc {
    contracts::UiRect rect;       // 整组绝对矩形；两行上下均分
    const char* labels[2];        // 两行文字，必须常驻
    int32_t selected;             // 初始选中 0/1
    UiOptionGroupChangeFn on_change;
    void* user;
};

struct UiOptionGroup {
    contracts::UiRect rects[2];
    contracts::QolUiHandle handles[2];
    const char* labels[2];
    UiOptionGroupChangeFn on_change;
    void* user;
    int32_t selected;
};

QolResult option_group_create(const contracts::QolUiComponents* components,
                              contracts::QolUiHandle parent,
                              const UiOptionGroupDesc* desc, UiOptionGroup* out);
void option_group_draw(const contracts::QolUiAdapter* adapter, const UiOptionGroup* group);
void option_group_destroy(const contracts::QolUiComponents* components, UiOptionGroup* group);

}  // namespace qol::features::core_ui
