#include "cards.h"

#include "../panel_internal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace qol::features::item_editor {
namespace {
using namespace contracts;
using namespace panel_internal;

QolUiHandle g_item_card_root = nullptr;
QolUiHandle g_item_card_footer_root = nullptr;
QolUiHandle g_item_card_list = nullptr;
QolUiHandle g_item_card_confirm = nullptr;
QolUiHandle g_item_card_cancel = nullptr;
std::vector<int32_t> g_item_card_ids;
std::vector<int32_t> g_item_card_levels;
std::vector<QolUiHandle> g_item_card_buttons;
constexpr size_t kItemCardSlotPoolSize = 12;
std::array<QolUiHandle, kItemCardSlotPoolSize> g_item_card_slots{};
int32_t g_item_card_selected_id = -1;
bool g_item_card_open_requested = false;
enum class ItemCardAction : uint8_t { kNone, kConfirm, kCancel };
ItemCardAction g_item_card_pending_action = ItemCardAction::kNone;

bool parse_item_ids(const char* csv, std::vector<int32_t>* out_ids) {
    if (csv == nullptr || out_ids == nullptr) return false;
    out_ids->clear();
    const char* cursor = csv;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
            ++cursor;
        if (*cursor == '\0') break;
        if (*cursor == ',') return false;
        errno = 0;
        char* end = nullptr;
        const long parsed = std::strtol(cursor, &end, 10);
        if (end == cursor || errno == ERANGE || parsed < 0 || parsed > INT32_MAX)
            return false;
        out_ids->push_back(static_cast<int32_t>(parsed));
        cursor = end;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
            ++cursor;
        if (*cursor == ',') {
            ++cursor;
            const char* next = cursor;
            while (*next == ' ' || *next == '\t' || *next == '\r' || *next == '\n')
                ++next;
            if (*next == '\0') return false;
        } else if (*cursor != '\0') {
            return false;
        }
    }
    return true;
}
void item_card_noop_draw(void*) {}

void item_card_cell_clicked(void* user) {
    const intptr_t encoded = reinterpret_cast<intptr_t>(user);
    if (encoded <= 0 || encoded - 1 > INT32_MAX) return;
    const int32_t item_id = static_cast<int32_t>(encoded - 1);
    g_item_card_selected_id = g_item_card_selected_id == item_id ? -1 : item_id;
}
void item_card_confirm_clicked(void*) {
    if (g_item_card_selected_id >= 0)
        g_item_card_pending_action = ItemCardAction::kConfirm;
}
void item_card_cancel_clicked(void*) {
    g_item_card_pending_action = ItemCardAction::kCancel;
}
void draw_item_card_row(void*, uint32_t row_index, QolUiHandle row, uint8_t) {
    if (g_item_card_ids.empty()) {
        draw_center_text(row, "该类别暂无物品", 364, 46, kMuted);
        return;
    }
    const uint32_t first = row_index * 4;
    for (uint32_t column = 0; column < 4; ++column) {
        const uint32_t item_index = first + column;
        if (item_index >= g_item_card_ids.size() ||
            item_index >= g_item_card_buttons.size() ||
            item_index >= g_item_card_levels.size()) break;
        const int32_t item_id = g_item_card_ids[item_index];
        const bool selected = item_id == g_item_card_selected_id;
        const char* name = static_text(QolItemStaticTextKind::kItemName,item_id);
        if (name == nullptr || name[0] == '\0') name = "未知";
        char truncated_name[64]{};
        char label[96]{};
        size_t input_bytes = std::strlen(name);
        size_t input_offset = 0;
        size_t output_bytes = 0;
        uint32_t codepoints = 0;
        bool truncated = false;
        while (input_offset < input_bytes && codepoints < 8) {
            const unsigned char lead = static_cast<unsigned char>(name[input_offset]);
            size_t sequence = lead < 0x80 ? 1u :
                ((lead & 0xE0) == 0xC0 ? 2u :
                 ((lead & 0xF0) == 0xE0 ? 3u : ((lead & 0xF8) == 0xF0 ? 4u : 1u)));
            if (sequence > input_bytes - input_offset) sequence = 1;
            if (output_bytes + sequence + 4 >= sizeof(truncated_name)) {
                truncated = true;
                break;
            }
            std::memcpy(truncated_name + output_bytes,name + input_offset,sequence);
            output_bytes += sequence;
            input_offset += sequence;
            ++codepoints;
        }
        if (input_offset < input_bytes) truncated = true;
        if (truncated) {
            truncated_name[output_bytes++] = static_cast<char>(0xE2);
            truncated_name[output_bytes++] = static_cast<char>(0x80);
            truncated_name[output_bytes++] = static_cast<char>(0xA6);
        }
        truncated_name[output_bytes] = '\0';
        if (item_class_is_equipment(g_draft.class_index)) {
            if (g_item_card_levels[item_index] >= 0) {
                std::snprintf(label,sizeof(label),"%s(%d)",truncated_name,
                    g_item_card_levels[item_index]);
            } else {
                std::snprintf(label,sizeof(label),"%s(?)",truncated_name);
            }
        } else {
            std::snprintf(label,sizeof(label),"%s",truncated_name);
        }
        draw_center_text(row,label,15 + static_cast<int32_t>(column) * 176 + 87, 81,
                         selected ? kGold : kText);
    }
}
void draw_item_card_footer_button(QolUiHandle button,const char* label,bool enabled) {
    const auto* adapter = ui();
    if (adapter == nullptr || button == nullptr) return;
    int64_t abs_x = 0,abs_y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(button,&abs_x,&abs_y))) return;
    int32_t pressed = 0;
    if (enabled && adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(button,&pressed);
    if (adapter->draw_help_option_row != nullptr)
        (void)adapter->draw_help_option_row({abs_x,abs_y,140,48},0,
            static_cast<uint8_t>(pressed != 0));
    draw_center_text(button,label,70,14,enabled ? kText : kMuted);
}
void draw_item_card_slots(const QolUiComponents* c, const QolUiAdapter* adapter,
                          const UiRect& panel, int64_t offset) {
    if (c == nullptr || adapter == nullptr || c->set_rect == nullptr ||
        c->item_slot_set == nullptr || c->item_slot_set_flags == nullptr ||
        c->item_slot_draw == nullptr) return;
    const int64_t view_x = panel.x + 70 + 16;
    const int64_t view_y = panel.y + 30 + 16;
    const int64_t view_w = 728;
    const int64_t view_h = 400;
    if (adapter->set_clip != nullptr)
        (void)adapter->set_clip(static_cast<int32_t>(view_x), static_cast<int32_t>(view_y),
                                static_cast<int32_t>(view_x + view_w),
                                static_cast<int32_t>(view_y + view_h));
    if (c->item_slot_set_quality != nullptr) {
        for (const QolUiHandle slot : g_item_card_slots) {
            if (slot != nullptr) (void)c->item_slot_set_quality(slot,-1);
        }
    }
    size_t pool_index = 0;
    for (size_t index = 0; index < g_item_card_ids.size() &&
                           pool_index < g_item_card_slots.size(); ++index) {
        const int32_t row = static_cast<int32_t>(index / 4);
        const int32_t column = static_cast<int32_t>(index % 4);
        const int64_t local_y = 16 + static_cast<int64_t>(row) * 122 - offset + 5;
        if (local_y + 74 <= 16 || local_y + 74 > 416) continue;
        const QolUiHandle slot = g_item_card_slots[pool_index++];
        if (slot == nullptr) continue;
        const int32_t item_id = g_item_card_ids[index];
        (void)c->set_rect(slot, {15 + column * 176 + 50,
                                 static_cast<int32_t>(local_y), 74, 74});
        (void)c->item_slot_set(slot, item_id, -1);
        const uint32_t flags = kUiItemSlotShowFrame |
            (item_id == g_item_card_selected_id ? kUiItemSlotSelected : 0u);
        (void)c->item_slot_set_flags(slot, flags);
        (void)c->item_slot_draw(slot);
    }
    for (; pool_index < g_item_card_slots.size(); ++pool_index) {
        if (g_item_card_slots[pool_index] != nullptr)
            (void)c->set_rect(g_item_card_slots[pool_index], {0, -200, 74, 74});
    }
    if (adapter->reset_clip != nullptr) (void)adapter->reset_clip();
}

}  // namespace

void item_card_open() {
    if (!item_class_valid(g_draft.class_index)) return;
    g_item_card_open_requested = true;
}
bool item_card_open_requested() { return g_item_card_open_requested; }
void item_card_clear_open_request() { g_item_card_open_requested = false; }
bool item_card_active() { return g_item_card_root != nullptr; }

bool item_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                            uint32_t* consumed) {
    const auto* c = components();
    if (c == nullptr || c->dispatch_native_event == nullptr) return false;
    // 先 footer 后 root；footer 未消费时事件继续落到 root，与拆分前一致。
    uint32_t local = 0;
    bool dispatched = false;
    if (g_item_card_footer_root != nullptr) {
        (void)c->dispatch_native_event(g_item_card_footer_root,event,param,param2,&local);
        dispatched = true;
        if (local != 0) {
            if (consumed != nullptr) *consumed = local;
            return true;
        }
    }
    if (g_item_card_root != nullptr) {
        (void)c->dispatch_native_event(g_item_card_root,event,param,param2,&local);
        dispatched = true;
    }
    if (consumed != nullptr) *consumed = local;
    return dispatched;
}

void destroy_item_card() {
    const auto* c = components();
    if (c != nullptr && c->destroy != nullptr) {
        if (g_item_card_list != nullptr) (void)c->destroy(g_item_card_list);
        if (g_item_card_confirm != nullptr) (void)c->destroy(g_item_card_confirm);
        if (g_item_card_cancel != nullptr) (void)c->destroy(g_item_card_cancel);
        if (g_item_card_footer_root != nullptr) (void)c->destroy(g_item_card_footer_root);
        for (auto& slot : g_item_card_slots) {
            if (slot != nullptr) (void)c->destroy(slot);
            slot = nullptr;
        }
        if (g_item_card_root != nullptr) (void)c->destroy(g_item_card_root);
    }
    g_item_card_root = nullptr;
    g_item_card_footer_root = nullptr;
    g_item_card_list = nullptr;
    g_item_card_confirm = nullptr;
    g_item_card_cancel = nullptr;
    for (auto& slot : g_item_card_slots) slot = nullptr;
    g_item_card_ids.clear();
    g_item_card_levels.clear();
    g_item_card_buttons.clear();
    g_item_card_selected_id = -1;
    g_item_card_open_requested = false;
    g_item_card_pending_action = ItemCardAction::kNone;
}
bool create_item_card() {
    const auto* c = components();
    const auto* adapter = ui();
    const auto* port = item_port();
    if (c == nullptr || adapter == nullptr || port == nullptr ||
        c->panel_create == nullptr || c->scroll_list_create == nullptr ||
        c->scroll_row_button_create == nullptr || c->button_create == nullptr ||
        c->scroll_list_set_selection_enabled == nullptr || c->item_slot_create == nullptr ||
        c->item_slot_set == nullptr || c->item_slot_set_flags == nullptr ||
        adapter->set_draw_proc == nullptr || !item_class_valid(g_draft.class_index) ||
        port->read.draw_icon_id == nullptr ||
        port->read.read_item_base_stats_ex == nullptr) {
        log_panel("op=item_card_create result=failed stage=precondition code=-1");
        return false;
    }
    const char* item_list = static_text(QolItemStaticTextKind::kItemList,g_draft.class_index);
    if (!parse_item_ids(item_list,&g_item_card_ids)) {
        log_panel("op=item_card_create result=failed stage=item_list_parse code=-1");
        return false;
    }
    g_item_card_levels.resize(g_item_card_ids.size(),-1);
    for (size_t index = 0; index < g_item_card_ids.size(); ++index) {
        QolItemBaseStats stats{};
        if (succeeded(port->read.read_item_base_stats_ex(
                g_item_card_ids[index],g_draft.raw_rarity,&stats))) {
            g_item_card_levels[index] = stats.need_level;
        }
    }
    std::vector<size_t> order(g_item_card_ids.size());
    for (size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(),order.end(),[](size_t lhs,size_t rhs) {
        const int32_t left_level = g_item_card_levels[lhs] < 0
            ? INT32_MAX : g_item_card_levels[lhs];
        const int32_t right_level = g_item_card_levels[rhs] < 0
            ? INT32_MAX : g_item_card_levels[rhs];
        if (left_level != right_level) return left_level < right_level;
        return g_item_card_ids[lhs] < g_item_card_ids[rhs];
    });
    std::vector<int32_t> sorted_ids;
    std::vector<int32_t> sorted_levels;
    sorted_ids.reserve(order.size());
    sorted_levels.reserve(order.size());
    for (size_t index : order) {
        sorted_ids.push_back(g_item_card_ids[index]);
        sorted_levels.push_back(g_item_card_levels[index]);
    }
    g_item_card_ids.swap(sorted_ids);
    g_item_card_levels.swap(sorted_levels);
    uint32_t rows = 1;
    if (!g_item_card_ids.empty())
        rows = static_cast<uint32_t>((g_item_card_ids.size() + 3u) / 4u);
    const UiRect panel = panel_rect();
    const UiPanelDesc card{{panel.x+70,panel.y+30,760,500},{0,0,760,500},0,0};
    QolResult result = c->panel_create(&card,nullptr,&g_item_card_root);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=card_panel code=%d",result.code);
        log_panel(message);
        return false;
    }
    // 可见格池使用真正的 core.custom_ui item_slot 组件；池先建，列表行按钮后建，
    // 触摸仍优先命中行按钮。每帧按滚动位移重排并只绘制可见项。
    for (auto& slot : g_item_card_slots) {
        const UiItemSlotDesc desc{{0,-200,74,74},-1,-1,kUiItemSlotShowFrame,-1};
        result = c->item_slot_create(&desc,g_item_card_root,&slot);
        if (!succeeded(result)) {
            char message[128]{};
            std::snprintf(message,sizeof(message),
                "op=item_card_create result=failed stage=slot_pool code=%d",result.code);
            log_panel(message);
            return false;
        }
    }
    const UiScrollListDesc list{{16,16,728,400},122,rows,-1,
        &draw_item_card_row,nullptr,nullptr};
    result = c->scroll_list_create(&list,g_item_card_root,&g_item_card_list);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=card_list code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = c->scroll_list_set_selection_enabled(g_item_card_list,0);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=list_selection code=%d",result.code);
        log_panel(message);
        return false;
    }
    g_item_card_buttons.assign(g_item_card_ids.size(),nullptr);
    for (uint32_t index = 0; index < g_item_card_ids.size(); ++index) {
        const uint32_t row = index / 4;
        const int32_t column = static_cast<int32_t>(index % 4);
        const UiRect cell{15 + column * 176,2,174,112};
        const intptr_t encoded = static_cast<intptr_t>(g_item_card_ids[index]) + 1;
        result = c->scroll_row_button_create(g_item_card_list,row,cell,
            &item_card_cell_clicked,reinterpret_cast<void*>(encoded),
            &g_item_card_buttons[index]);
        if (!succeeded(result)) {
            char message[128]{};
            std::snprintf(message,sizeof(message),
                "op=item_card_create result=failed stage=cell_button code=%d index=%u",
                result.code,index);
            log_panel(message);
            return false;
        }
        result = adapter->set_draw_proc(g_item_card_buttons[index],&item_card_noop_draw);
        if (!succeeded(result)) {
            char message[128]{};
            std::snprintf(message,sizeof(message),
                "op=item_card_create result=failed stage=cell_draw code=%d index=%u",
                result.code,index);
            log_panel(message);
            return false;
        }
    }
    g_item_card_selected_id = -1;
    const auto selected_item = std::find(
        g_item_card_ids.begin(),g_item_card_ids.end(),g_draft.item_id);
    if (draft_has_item(g_draft) && selected_item != g_item_card_ids.end()) {
        g_item_card_selected_id = g_draft.item_id;
        if (c->scroll_set_offset != nullptr) {
            const auto index = static_cast<uint32_t>(
                std::distance(g_item_card_ids.begin(),selected_item));
            const int32_t row_top = static_cast<int32_t>((index / 4) * 122);
            const int32_t offset = std::max(0,row_top - (400 - 122) / 2);
            (void)c->scroll_set_offset(g_item_card_list,offset);
        }
    }
    const UiPanelDesc footer{{panel.x+70,panel.y+466,760,48},{0,0,760,48},0,0};
    result = c->panel_create(&footer,nullptr,&g_item_card_footer_root);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=footer_panel code=%d",result.code);
        log_panel(message);
        return false;
    }
    const UiButtonDesc confirm{{230,0,140,48},"",0,
        &item_card_confirm_clicked,nullptr};
    result = c->button_create(&confirm,g_item_card_footer_root,&g_item_card_confirm);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=confirm code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_item_card_confirm,&item_card_noop_draw);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=confirm_draw code=%d",result.code);
        log_panel(message);
        return false;
    }
    const UiButtonDesc cancel{{394,0,140,48},"",0,
        &item_card_cancel_clicked,nullptr};
    result = c->button_create(&cancel,g_item_card_footer_root,&g_item_card_cancel);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=cancel code=%d",result.code);
        log_panel(message);
        return false;
    }
    result = adapter->set_draw_proc(g_item_card_cancel,&item_card_noop_draw);
    if (!succeeded(result)) {
        char message[128]{};
        std::snprintf(message,sizeof(message),
            "op=item_card_create result=failed stage=cancel_draw code=%d",result.code);
        log_panel(message);
        return false;
    }
    return true;
}
void draw_item_card() {
    if (g_item_card_root == nullptr) return;
    const auto* adapter = ui();
    const auto* c = components();
    if (adapter == nullptr || c == nullptr) return;
    const UiRect panel = panel_rect();
    if (adapter->fill_rect_alpha != nullptr)
        (void)adapter->fill_rect_alpha({panel.x,panel.y,panel.w,panel.h},0x0000,40);
    if (c->draw_container_styled != nullptr) {
        const UiContainerStyleDesc card{{panel.x+70,panel.y+30,760,500},0x0000,60,1};
        (void)c->draw_container_styled(&card);
    }
    (void)c->scroll_list_process(g_item_card_list);
    (void)c->scroll_list_draw(g_item_card_list);
    int64_t offset = 0;
    if (c->scroll_list_get_offset != nullptr)
        (void)c->scroll_list_get_offset(g_item_card_list, &offset);
    draw_item_card_slots(c, adapter, panel, offset < 0 ? 0 : offset);
    draw_item_card_footer_button(g_item_card_confirm,"确认",
        g_item_card_selected_id >= 0);
    draw_item_card_footer_button(g_item_card_cancel,"取消",true);
}
void item_card_apply() {
    if (g_item_card_pending_action == ItemCardAction::kNone) return;
    const ItemCardAction action = g_item_card_pending_action;
    g_item_card_pending_action = ItemCardAction::kNone;
    bool changed = false;
    if (action == ItemCardAction::kConfirm && g_item_card_selected_id >= 0) {
        g_draft.item_id = g_item_card_selected_id;
        g_draft.item_type = item_type_for_class(g_draft.class_index);
        const auto* port = item_port();
        if (port != nullptr && port->read.read_item_base_stats_ex != nullptr) {
            QolItemBaseStats stats{};
            const QolResult result =
                port->read.read_item_base_stats_ex(
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
        g_draft.dirty = true;
        changed = true;
    }
    destroy_item_card();
    if (changed) {
        build_left_rows();
        rebuild_left_list();
        rebuild_right_list();
        rebuild_item_slot();
    }
}

}  // namespace qol::features::item_editor
