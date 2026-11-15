#include "cards.h"

#include "../panel_internal.h"
#include "../../../../contracts/game_ports/item_port.h"

#include <cstdio>
#include <cstring>

namespace qol::features::item_editor {
namespace {
using namespace contracts;
using namespace panel_internal;

// 删除词条确认卡：420×250 无标题；确认后从对应 draft 数组删除并压缩。
// 生命周期与其它卡片一致：open 只置请求，panel_process 安全点 create/apply。

constexpr int32_t kCardW = 420;
constexpr int32_t kCardH = 250;

QolUiHandle g_card_root = nullptr;
QolUiHandle g_confirm = nullptr;
QolUiHandle g_cancel = nullptr;
bool g_open_requested = false;
uint32_t g_section = 0;
uint32_t g_index = 0;
enum class DeleteCardAction : uint8_t { kNone, kConfirm, kCancel };
DeleteCardAction g_pending_action = DeleteCardAction::kNone;

void noop_draw(void*) {}
void confirm_clicked(void*) { g_pending_action = DeleteCardAction::kConfirm; }
void cancel_clicked(void*) { g_pending_action = DeleteCardAction::kCancel; }

void draw_card_button(QolUiHandle button, const char* label, int32_t width,
                      int32_t height) {
    const auto* adapter = ui();
    if (adapter == nullptr || button == nullptr) return;
    int64_t abs_x = 0, abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(button, &abs_x, &abs_y))) return;
    int32_t pressed = 0;
    if (adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(button, &pressed);
    if (adapter->draw_help_option_row != nullptr)
        (void)adapter->draw_help_option_row({abs_x, abs_y, width, height}, 0,
                                            static_cast<uint8_t>(pressed != 0));
    draw_center_text(button, label, width / 2, height / 2 - 8, kText);
}

// 从 draft 数组删除词条并压缩；只改 draft。删除宝石词条不回缩孔数。
void erase_option(uint32_t section, uint32_t index) {
    QolItemInventoryOptionView* options = nullptr;
    uint32_t* count = nullptr;
    if (section == 0 && index < g_draft.native_option_count) {
        options = g_draft.native_options;
        count = &g_draft.native_option_count;
    } else if (section == 1 && index < g_draft.jewel_option_count) {
        // 宝石本体段没有删除入口；这里只可能删装备上的宝石词条。
        if (g_draft.class_index == kItemClassGem) return;
        options = g_draft.jewel_options;
        count = &g_draft.jewel_option_count;
    } else if (section == 2 && index < g_draft.enchant_option_count) {
        options = g_draft.enchant_options;
        count = &g_draft.enchant_option_count;
    } else {
        return;
    }
    std::memmove(options + index, options + index + 1,
                 (*count - index - 1) * sizeof(QolItemInventoryOptionView));
    --*count;
    g_draft.dirty = true;
    rebuild_right_list();
}

}  // namespace

void delete_option_card_open(uint32_t section, uint32_t index) {
    if (section > 2) return;
    g_section = section;
    g_index = index;
    g_open_requested = true;
}
bool delete_option_card_open_requested() { return g_open_requested; }
void delete_option_card_clear_open_request() { g_open_requested = false; }
bool delete_option_card_active() { return g_card_root != nullptr; }

bool delete_option_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                                     uint32_t* consumed) {
    const auto* c = components();
    if (c == nullptr || c->dispatch_native_event == nullptr || g_card_root == nullptr)
        return false;
    // 确认卡存活时吞掉全部事件，不落到主面板。
    (void)c->dispatch_native_event(g_card_root, event, param, param2, consumed);
    return true;
}

void destroy_delete_option_card() {
    const auto* c = components();
    if (c != nullptr && c->destroy != nullptr) {
        if (g_confirm != nullptr) (void)c->destroy(g_confirm);
        if (g_cancel != nullptr) (void)c->destroy(g_cancel);
        if (g_card_root != nullptr) (void)c->destroy(g_card_root);
    }
    g_confirm = nullptr;
    g_cancel = nullptr;
    g_card_root = nullptr;
    g_open_requested = false;
    g_pending_action = DeleteCardAction::kNone;
}

bool create_delete_option_card() {
    const auto* c = components();
    const auto* adapter = ui();
    if (c == nullptr || adapter == nullptr || c->panel_create == nullptr ||
        c->button_create == nullptr || adapter->set_draw_proc == nullptr) {
        log_panel("op=delete_option_card_create result=failed stage=precondition code=-1");
        return false;
    }
    const auto fail = [](const char* stage, QolResult result) {
        char message[112]{};
        std::snprintf(message, sizeof(message),
            "op=delete_option_card_create result=failed stage=%s code=%d",
            stage, result.code);
        log_panel(message);
        return false;
    };
    const UiRect panel = panel_rect();
    const UiRect card_rect{panel.x + (panel.w - kCardW) / 2,
        panel.y + (panel.h - kCardH) / 2, kCardW, kCardH};
    const UiPanelDesc card{card_rect, {0, 0, kCardW, kCardH}, 0, 0};
    QolResult result = c->panel_create(&card, nullptr, &g_card_root);
    if (!succeeded(result)) return fail("card_root", result);
    const UiButtonDesc confirm{{60, 170, 140, 48}, "", 0, &confirm_clicked, nullptr};
    result = c->button_create(&confirm, g_card_root, &g_confirm);
    if (!succeeded(result)) return fail("confirm_button", result);
    result = adapter->set_draw_proc(g_confirm, &noop_draw);
    if (!succeeded(result)) return fail("confirm_draw", result);
    const UiButtonDesc cancel{{220, 170, 140, 48}, "", 0, &cancel_clicked, nullptr};
    result = c->button_create(&cancel, g_card_root, &g_cancel);
    if (!succeeded(result)) return fail("cancel_button", result);
    result = adapter->set_draw_proc(g_cancel, &noop_draw);
    if (!succeeded(result)) return fail("cancel_draw", result);
    return true;
}

void draw_delete_option_card() {
    if (g_card_root == nullptr) return;
    const auto* adapter = ui();
    const auto* c = components();
    if (adapter == nullptr || c == nullptr) return;
    const UiRect panel = panel_rect();
    if (adapter->fill_rect_alpha != nullptr)
        (void)adapter->fill_rect_alpha({panel.x, panel.y, panel.w, panel.h}, 0x0000, 40);
    const UiRect card{panel.x + (panel.w - kCardW) / 2,
        panel.y + (panel.h - kCardH) / 2, kCardW, kCardH};
    if (c->draw_container_styled != nullptr) {
        const UiContainerStyleDesc style{card, 0x0000, 60, 1};
        (void)c->draw_container_styled(&style);
    }
    draw_center_text(g_card_root, "删除这条词条？", kCardW / 2, 80, kText);
    draw_card_button(g_confirm, "确定", 140, 48);
    draw_card_button(g_cancel, "取消", 140, 48);
}

void delete_option_card_apply() {
    if (g_pending_action == DeleteCardAction::kNone) return;
    const DeleteCardAction action = g_pending_action;
    g_pending_action = DeleteCardAction::kNone;
    if (action == DeleteCardAction::kConfirm) erase_option(g_section, g_index);
    destroy_delete_option_card();
}

}  // namespace qol::features::item_editor
