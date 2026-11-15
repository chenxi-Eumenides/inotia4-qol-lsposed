#include "cards.h"

#include "../panel_internal.h"
#include "../../../../contracts/item_equipment_limits.h"
#include "../../../../contracts/item_display_name.h"

#include <cstdio>
#include <cstring>

namespace qol::features::item_editor {
namespace {
using namespace contracts;
using namespace panel_internal;

QolUiHandle g_prefix_card_root = nullptr;
QolUiHandle g_prefix_buttons[15]{};
int32_t g_prefix_option_raw[15]{};
uint32_t g_prefix_option_count = 0;
QolUiHandle g_prefix_confirm = nullptr;
QolUiHandle g_prefix_cancel = nullptr;
bool g_prefix_card_open_requested = false;
int32_t g_prefix_selected_raw = -1;
enum class PrefixCardAction : uint8_t { kNone, kConfirm, kCancel };
PrefixCardAction g_prefix_card_pending_action = PrefixCardAction::kNone;

void noop_draw(void*) {}
void prefix_option_clicked(void* user) {
    const auto* raw = static_cast<const int32_t*>(user);
    if (raw == nullptr || *raw < 0 || *raw > 14) return;
    g_prefix_selected_raw = *raw;
}
void prefix_confirm_clicked(void*) {
    if (g_prefix_selected_raw >= 0)
        g_prefix_card_pending_action = PrefixCardAction::kConfirm;
}
void prefix_cancel_clicked(void*) {
    g_prefix_card_pending_action = PrefixCardAction::kCancel;
}
void build_prefix_options() {
    // raw 0..14 全部可选；空名称表示该变种/类别没有对应前缀，但 raw 仍决定品质。
    g_prefix_option_count = 0;
    for (int32_t raw = 0; raw <= 14; ++raw)
        g_prefix_option_raw[g_prefix_option_count++] = raw;
}
bool prefix_raw_available(int32_t raw) {
    for (uint32_t index = 0; index < g_prefix_option_count; ++index) {
        if (g_prefix_option_raw[index] == raw) return true;
    }
    return false;
}
void draw_prefix_card_button(QolUiHandle button,const char* label,
                             int32_t width,int32_t height,bool selected,
                             bool enabled = true) {
    const auto* adapter = ui();
    if (adapter == nullptr || button == nullptr) return;
    int64_t abs_x = 0,abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(button,&abs_x,&abs_y))) return;
    int32_t pressed = 0;
    if (enabled && adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(button,&pressed);
    if (adapter->draw_help_option_row != nullptr)
        (void)adapter->draw_help_option_row({abs_x,abs_y,width,height},
            static_cast<uint8_t>(selected),static_cast<uint8_t>(pressed != 0));
    draw_center_text(button,label,width/2,height/2-8,
        selected ? kGold : (enabled ? kText : kMuted));
}

}  // namespace

void prefix_card_open() {
    if (g_draft.prefix_class_bits == 0) return;
    resolve_variant_series();
    g_prefix_card_open_requested = true;
}
bool prefix_card_open_requested() { return g_prefix_card_open_requested; }
void prefix_card_clear_open_request() { g_prefix_card_open_requested = false; }
bool prefix_card_active() { return g_prefix_card_root != nullptr; }

bool prefix_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                              uint32_t* consumed) {
    const auto* c = components();
    if (c == nullptr || c->dispatch_native_event == nullptr ||
        g_prefix_card_root == nullptr) return false;
    // 前缀卡只有单 root；存活时吞掉全部事件，不落到主面板。
    (void)c->dispatch_native_event(g_prefix_card_root,event,param,param2,consumed);
    return true;
}

void destroy_prefix_card() {
    const auto* c = components();
    if (c != nullptr && c->destroy != nullptr) {
        for (auto& button : g_prefix_buttons) {
            if (button != nullptr) (void)c->destroy(button);
            button = nullptr;
        }
        if (g_prefix_confirm != nullptr) (void)c->destroy(g_prefix_confirm);
        if (g_prefix_cancel != nullptr) (void)c->destroy(g_prefix_cancel);
        if (g_prefix_card_root != nullptr) (void)c->destroy(g_prefix_card_root);
    }
    for (auto& button : g_prefix_buttons) button = nullptr;
    g_prefix_confirm = nullptr;
    g_prefix_cancel = nullptr;
    g_prefix_card_root = nullptr;
    g_prefix_option_count = 0;
    g_prefix_selected_raw = -1;
    g_prefix_card_open_requested = false;
    g_prefix_card_pending_action = PrefixCardAction::kNone;
}
bool create_prefix_card() {
    const auto* c = components();
    const auto* adapter = ui();
    if (c == nullptr || adapter == nullptr || c->panel_create == nullptr ||
        c->button_create == nullptr || adapter->set_draw_proc == nullptr ||
        g_draft.prefix_class_bits == 0) {
        log_panel("op=prefix_card_create result=failed stage=precondition code=-1");
        return false;
    }
    resolve_variant_series();
    build_prefix_options();
    g_prefix_selected_raw = prefix_raw_available(g_draft.raw_rarity)
        ? g_draft.raw_rarity : -1;
    const UiRect panel = panel_rect();
    const UiRect card_rect{panel.x+(panel.w-760)/2,panel.y+(panel.h-400)/2,760,400};
    const UiPanelDesc card{card_rect,{0,0,760,400},0,0};
    QolResult result = c->panel_create(&card,nullptr,&g_prefix_card_root);
    if (!succeeded(result)) {
        char message[112]{};
        std::snprintf(message,sizeof(message),
            "op=prefix_card_create result=failed stage=card_root code=%d",result.code);
        log_panel(message);
        return false;
    }
    int32_t rows_by_quality[5]{};
    for (uint32_t index = 0; index < g_prefix_option_count; ++index) {
        const int32_t raw = g_prefix_option_raw[index];
        const int32_t quality = contracts::item_rarity_from_raw_prefix(raw);
        if (quality < 0 || quality > 4) continue;
        const int32_t column = quality;
        const int32_t row = rows_by_quality[quality]++;
        const UiButtonDesc button{{14+column*146,54+row*50,138,42},"",0,
            &prefix_option_clicked,&g_prefix_option_raw[index]};
        result = c->button_create(&button,g_prefix_card_root,&g_prefix_buttons[index]);
        if (!succeeded(result)) {
            char message[112]{};
            std::snprintf(message,sizeof(message),
                "op=prefix_card_create result=failed stage=option_button code=%d index=%u",
                result.code,index);
            log_panel(message);
            return false;
        }
        result = adapter->set_draw_proc(g_prefix_buttons[index],&noop_draw);
        if (!succeeded(result)) {
            char message[112]{};
            std::snprintf(message,sizeof(message),
                "op=prefix_card_create result=failed stage=option_draw code=%d index=%u",
                result.code,index);
            log_panel(message);
            return false;
        }
    }
    const UiButtonDesc confirm{{250,340,110,42},"",0,
        &prefix_confirm_clicked,nullptr};
    result = c->button_create(&confirm,g_prefix_card_root,&g_prefix_confirm);
    if (!succeeded(result)) {
        char message[112]{};
        std::snprintf(message,sizeof(message),
            "op=prefix_card_create result=failed stage=confirm_button code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_prefix_confirm,&noop_draw);
    if (!succeeded(result)) {
        log_panel("op=prefix_card_create result=failed stage=confirm_draw code=-1");
        return false;
    }
    const UiButtonDesc cancel{{400,340,110,42},"",0,
        &prefix_cancel_clicked,nullptr};
    result = c->button_create(&cancel,g_prefix_card_root,&g_prefix_cancel);
    if (!succeeded(result)) {
        char message[112]{};
        std::snprintf(message,sizeof(message),
            "op=prefix_card_create result=failed stage=cancel_button code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_prefix_cancel,&noop_draw);
    if (!succeeded(result)) {
        log_panel("op=prefix_card_create result=failed stage=cancel_draw code=-1");
        return false;
    }
    return true;
}
void draw_prefix_card() {
    if (g_prefix_card_root == nullptr) return;
    const auto* adapter = ui();
    const auto* c = components();
    if (adapter == nullptr || c == nullptr) return;
    const UiRect panel = panel_rect();
    const UiRect card{panel.x+(panel.w-760)/2,panel.y+(panel.h-400)/2,760,400};
    if (adapter->fill_rect_alpha != nullptr)
        (void)adapter->fill_rect_alpha({panel.x,panel.y,panel.w,panel.h},0x0000,40);
    if (c->draw_container_styled != nullptr) {
        const UiContainerStyleDesc style{card,0x0000,60,1};
        (void)c->draw_container_styled(&style);
    }
    static constexpr const char* kQualityNames[5] = {
        "白装", "绿装", "蓝装", "黄装", "紫装"};
    for (int32_t quality = 0; quality < 5; ++quality) {
        draw_text(g_prefix_card_root,kQualityNames[quality],
            14+quality*146+69,20,kGold,2,0);
    }
    int32_t rows_by_quality[5]{};
    for (uint32_t index = 0; index < g_prefix_option_count; ++index) {
        const int32_t raw = g_prefix_option_raw[index];
        const int32_t quality = contracts::item_rarity_from_raw_prefix(raw);
        if (quality < 0 || quality > 4) continue;
        const int32_t row = rows_by_quality[quality]++;
        const char* prefix = raw == 2 ? "" : contracts::item_rarity_prefix(
            g_draft.variant_series,g_draft.prefix_class_bits,raw);
        if (prefix == nullptr) prefix = "";
        size_t trimmed = std::strlen(prefix);
        while (trimmed > 0 && prefix[trimmed - 1] == ' ') --trimmed;
        char label[64]{};
        if (trimmed == 0) std::snprintf(label,sizeof(label),"无前缀 (%d)",raw);
        else std::snprintf(label,sizeof(label),"%.*s (%d)",
            static_cast<int>(trimmed),prefix,raw);
        draw_prefix_card_button(g_prefix_buttons[index],label,138,42,
            raw == g_prefix_selected_raw);
    }
    draw_prefix_card_button(g_prefix_confirm,"确定",110,42,false,
        g_prefix_selected_raw >= 0);
    draw_prefix_card_button(g_prefix_cancel,"取消",110,42,false,true);
}
void prefix_card_apply() {
    if (g_prefix_card_pending_action == PrefixCardAction::kNone) return;
    const PrefixCardAction action = g_prefix_card_pending_action;
    g_prefix_card_pending_action = PrefixCardAction::kNone;
    const bool confirmed = action == PrefixCardAction::kConfirm &&
        prefix_raw_available(g_prefix_selected_raw);
    if (confirmed) {
        g_draft.raw_rarity = g_prefix_selected_raw;
        g_draft.dirty = true;
        if (draft_has_item(g_draft)) {
            const auto* port = item_port();
            if (port != nullptr && port->read.read_item_base_stats_ex != nullptr) {
                QolItemBaseStats stats{};
                const QolResult result = port->read.read_item_base_stats_ex(
                    g_draft.item_id,g_draft.raw_rarity,&stats);
                if (succeeded(result)) {
                    g_draft.damage = stats.damage;
                    g_draft.defense = stats.defense;
                    g_draft.magic_rate = stats.magic_rate;
                    g_draft.need_level = stats.need_level;
                    g_draft.stackable = stats.stackable != 0;
                    if (!g_draft.stackable) g_draft.count = 1;
                } else {
                    char message[128]{};
                    std::snprintf(message,sizeof(message),
                        "fail item=%d op=load_item_stats result=failed stage=read code=%d reason=base_stats_read_failed",
                        g_draft.item_id,result.code);
                    log_panel(message);
                }
            } else {
                log_panel("fail op=load_item_stats result=failed stage=api code=-1 reason=api_unavailable");
            }
        }
    }
    destroy_prefix_card();
    if (confirmed) {
        build_left_rows();
        rebuild_left_list();
        rebuild_item_slot();
    }
}

}  // namespace qol::features::item_editor
