#include "cards.h"

#include "../panel_internal.h"

#include <algorithm>
#include <cstdio>

namespace qol::features::item_editor {
namespace {
using namespace contracts;
using namespace panel_internal;

QolUiHandle g_card_root = nullptr;
QolUiHandle g_card_footer_root = nullptr;
QolUiHandle g_card_list = nullptr;
QolUiHandle g_card_confirm = nullptr;
QolUiHandle g_card_cancel = nullptr;
QolUiHandle g_card_buttons[kItemClassCount]{};
int32_t g_card_button_classes[kItemClassCount]{};
int32_t g_card_selected_class = -1;
bool g_card_open_requested = false;
enum class CategoryCardAction : uint8_t { kNone, kConfirm, kCancel };
CategoryCardAction g_card_pending_action = CategoryCardAction::kNone;

void category_card_noop_draw(void*) {}
void category_class_clicked(void* user) {
    const auto* class_index = static_cast<const int32_t*>(user);
    if (class_index == nullptr || !item_class_valid(*class_index)) return;
    g_card_selected_class = g_card_selected_class == *class_index ? -1 : *class_index;
}
void category_card_confirm_clicked(void*) {
    if (item_class_valid(g_card_selected_class))
        g_card_pending_action = CategoryCardAction::kConfirm;
}
void category_card_cancel_clicked(void*) {
    g_card_pending_action = CategoryCardAction::kCancel;
}
bool category_card_row_classes(uint32_t row, int32_t* first, int32_t* count) {
    if (first == nullptr || count == nullptr) return false;
    if (row >= 1 && row <= 3) {
        *first = static_cast<int32_t>((row - 1) * 3);
        *count = 3;
        return true;
    }
    if (row >= 5 && row <= 9) {
        *first = 9 + static_cast<int32_t>(row - 5) * 3;
        *count = std::min(3, 22 - *first);
        return true;
    }
    if (row >= 11 && row <= 15) {
        *first = 22 + static_cast<int32_t>(row - 11) * 3;
        *count = std::min(3, kItemClassCount - *first);
        return true;
    }
    return false;
}
bool category_class_location(int32_t class_index, uint32_t* row, int32_t* column) {
    if (!item_class_valid(class_index) || row == nullptr || column == nullptr) return false;
    if (class_index <= 8) {
        *row = 1u + static_cast<uint32_t>(class_index / 3);
        *column = class_index % 3;
    } else if (class_index <= 21) {
        *row = 5u + static_cast<uint32_t>((class_index - 9) / 3);
        *column = (class_index - 9) % 3;
    } else {
        *row = 11u + static_cast<uint32_t>((class_index - 22) / 3);
        *column = (class_index - 22) % 3;
    }
    return true;
}
void draw_category_card_row(void*, uint32_t row_index, QolUiHandle row, uint8_t) {
    if (row_index == 0 || row_index == 4 || row_index == 10) {
        const char* title = row_index == 0 ? "武器" : row_index == 4 ? "装备" : "物品";
        draw_text(row, title, 304, 16, kGold, 2, 1);
        return;
    }
    int32_t first = 0, count = 0;
    if (!category_card_row_classes(row_index, &first, &count)) return;
    const auto* adapter = ui();
    if (adapter == nullptr) return;
    for (int32_t column = 0; column < count; ++column) {
        const int32_t class_index = first + column;
        const QolUiHandle button = g_card_buttons[class_index];
        if (button == nullptr) continue;
        int64_t abs_x = 0, abs_y = 0;
        if (adapter->control_absolute_origin == nullptr ||
            !succeeded(adapter->control_absolute_origin(button, &abs_x, &abs_y))) continue;
        int32_t pressed = 0;
        if (adapter->control_pressed != nullptr)
            (void)adapter->control_pressed(button, &pressed);
        (void)adapter->draw_help_option_row({abs_x, abs_y, 186, 48},
            static_cast<uint8_t>(g_card_selected_class == class_index),
            static_cast<uint8_t>(pressed != 0));
        const char* name = static_text(QolItemStaticTextKind::kItemClassList, class_index);
        if (name == nullptr || name[0] == '\0') name = "未知类别";
        draw_text(row, name, 15 + column * 196 + 93, 16,
            g_card_selected_class == class_index ? kGold : kText, 2, 0);
    }
}
void draw_category_card_footer_button(QolUiHandle button, const char* label,
                                      bool enabled) {
    const auto* adapter = ui();
    if (adapter == nullptr || button == nullptr) return;
    int64_t abs_x = 0, abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(button, &abs_x, &abs_y))) return;
    int32_t pressed = 0;
    if (enabled && adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(button, &pressed);
    (void)adapter->draw_help_option_row({abs_x, abs_y, 140, 48}, 0,
        static_cast<uint8_t>(pressed != 0));
    draw_text(button, label, 70, 14, enabled ? kText : kMuted, 2, 0);
}

}  // namespace

void category_card_open() { g_card_open_requested = true; }
bool category_card_open_requested() { return g_card_open_requested; }
void category_card_clear_open_request() { g_card_open_requested = false; }
bool category_card_active() { return g_card_root != nullptr; }

bool category_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                                uint32_t* consumed) {
    const auto* c = components();
    if (c == nullptr || c->dispatch_native_event == nullptr) return false;
    // 先 footer 后 root；footer 未消费时事件继续落到 root，与拆分前一致。
    uint32_t local = 0;
    bool dispatched = false;
    if (g_card_footer_root != nullptr) {
        (void)c->dispatch_native_event(g_card_footer_root,event,param,param2,&local);
        dispatched = true;
        if (local != 0) {
            if (consumed != nullptr) *consumed = local;
            return true;
        }
    }
    if (g_card_root != nullptr) {
        (void)c->dispatch_native_event(g_card_root,event,param,param2,&local);
        dispatched = true;
    }
    if (consumed != nullptr) *consumed = local;
    return dispatched;
}

void destroy_category_card() {
    const auto* c = components();
    if (c != nullptr && c->destroy != nullptr) {
        if (g_card_list != nullptr) (void)c->destroy(g_card_list);
        if (g_card_confirm != nullptr) (void)c->destroy(g_card_confirm);
        if (g_card_cancel != nullptr) (void)c->destroy(g_card_cancel);
        if (g_card_footer_root != nullptr) (void)c->destroy(g_card_footer_root);
        if (g_card_root != nullptr) (void)c->destroy(g_card_root);
    }
    g_card_root = nullptr;
    g_card_footer_root = nullptr;
    g_card_list = nullptr;
    g_card_confirm = nullptr;
    g_card_cancel = nullptr;
    for (auto& button : g_card_buttons) button = nullptr;
    g_card_open_requested = false;
    g_card_pending_action = CategoryCardAction::kNone;
}
bool create_category_card() {
    const auto* c = components();
    const auto* adapter = ui();
    if (c == nullptr || adapter == nullptr || c->panel_create == nullptr ||
        c->scroll_list_create == nullptr || c->scroll_row_button_create == nullptr ||
        c->button_create == nullptr || adapter->set_draw_proc == nullptr ||
        c->scroll_list_set_selection_enabled == nullptr) {
        log_panel("op=category_card_create result=failed stage=precondition code=-1");
        return false;
    }
    const auto fail = [](const char* stage, QolResult result) {
        char message[128]{};
        std::snprintf(message, sizeof(message),
            "op=category_card_create result=failed stage=%s code=%d", stage, result.code);
        log_panel(message);
        return false;
    };
    // panel_create 只接受空 parent；卡片作为独立 root 由 panel_event 单独派发。
    // 卡片根必须落在面板绝对原点，子控件与 draw_container_styled 才共用同一坐标系。
    const UiRect panel = panel_rect();
    const UiPanelDesc card_desc{{panel.x+130,panel.y+30,640,500},{0,0,640,500},0,0};
    QolResult result = c->panel_create(&card_desc, nullptr, &g_card_root);
    if (!succeeded(result)) return fail("card_panel", result);
    const UiScrollListDesc list{{16,16,608,400},52,16,-1,
        &draw_category_card_row,nullptr,nullptr};
    result = c->scroll_list_create(&list,g_card_root,&g_card_list);
    if (!succeeded(result)) return fail("card_list", result);
    result = c->scroll_list_set_selection_enabled(g_card_list,0);
    if (!succeeded(result)) return fail("card_selection", result);
    g_card_selected_class = item_class_valid(g_draft.class_index) ? g_draft.class_index : -1;
    for (int32_t class_index = 0; class_index < kItemClassCount; ++class_index) {
        uint32_t row = 0;
        int32_t column = 0;
        if (!category_class_location(class_index, &row, &column)) {
            log_panel("op=category_card_create result=failed stage=class_location code=-1");
            return false;
        }
        g_card_button_classes[class_index] = class_index;
        const UiRect rect{15 + column * 196,2,186,48};
        result = c->scroll_row_button_create(g_card_list,row,rect,
                &category_class_clicked,&g_card_button_classes[class_index],
                &g_card_buttons[class_index]);
        if (!succeeded(result)) {
            char message[96]{};
            std::snprintf(message, sizeof(message),
                "op=category_card_create result=failed stage=button code=%d class=%d",
                result.code, class_index);
            log_panel(message);
            return false;
        }
        result = adapter->set_draw_proc(g_card_buttons[class_index],
                &category_card_noop_draw);
        if (!succeeded(result)) {
            char message[96]{};
            std::snprintf(message, sizeof(message),
                "op=category_card_create result=failed stage=draw code=%d class=%d",
                result.code, class_index);
            log_panel(message);
            return false;
        }
    }
    // 列表的 scroll target group 会延伸到 viewport 外；底部按钮放到独立 root，
    // 事件先派发给它，避免被列表内容区的命中区吞掉。
    const UiPanelDesc footer_desc{{panel.x+130,panel.y+466,640,48},{0,0,640,48},0,0};
    result = c->panel_create(&footer_desc, nullptr, &g_card_footer_root);
    if (!succeeded(result)) return fail("card_footer", result);
    const UiButtonDesc confirm{{172,0,140,48},"",0,
        &category_card_confirm_clicked,nullptr};
    result = c->button_create(&confirm,g_card_footer_root,&g_card_confirm);
    if (!succeeded(result)) return fail("card_confirm", result);
    result = adapter->set_draw_proc(g_card_confirm,&category_card_noop_draw);
    if (!succeeded(result)) return fail("card_confirm_draw", result);
    const UiButtonDesc cancel{{328,0,140,48},"",0,
        &category_card_cancel_clicked,nullptr};
    result = c->button_create(&cancel,g_card_footer_root,&g_card_cancel);
    if (!succeeded(result)) return fail("card_cancel", result);
    result = adapter->set_draw_proc(g_card_cancel,&category_card_noop_draw);
    if (!succeeded(result)) return fail("card_cancel_draw", result);
    return true;
}
void draw_category_card() {
    if (g_card_root == nullptr) return;
    const auto* adapter = ui();
    const auto* c = components();
    if (adapter == nullptr || c == nullptr) return;
    const UiRect panel = panel_rect();
    if (adapter->fill_rect_alpha != nullptr)
        (void)adapter->fill_rect_alpha({panel.x,panel.y,panel.w,panel.h},0x0000,40);
    if (c->draw_container_styled != nullptr) {
        const UiContainerStyleDesc card{{panel.x+130,panel.y+30,640,500},0x0000,60,1};
        (void)c->draw_container_styled(&card);
    }
    (void)c->scroll_list_process(g_card_list);
    (void)c->scroll_list_draw(g_card_list);
    draw_category_card_footer_button(g_card_confirm,"确认",
        item_class_valid(g_card_selected_class));
    draw_category_card_footer_button(g_card_cancel,"取消",true);
}
void category_card_apply() {
    if (g_card_pending_action == CategoryCardAction::kNone) return;
    const CategoryCardAction action = g_card_pending_action;
    g_card_pending_action = CategoryCardAction::kNone;
    bool changed = false;
    if (action == CategoryCardAction::kConfirm &&
        item_class_valid(g_card_selected_class)) {
        changed = draft_change_class(&g_draft,g_card_selected_class);
    }
    destroy_category_card();
    if (changed) {
        build_left_rows();
        rebuild_left_list();
        rebuild_right_list();
        rebuild_item_slot();
    }
}

}  // namespace qol::features::item_editor
