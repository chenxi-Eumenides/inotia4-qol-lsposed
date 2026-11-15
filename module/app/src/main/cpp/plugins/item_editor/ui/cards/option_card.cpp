#include "cards.h"

#include "../panel_internal.h"
#include "../../../../contracts/game_ports/item_port.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace qol::features::item_editor {
namespace {
using namespace contracts;
using namespace panel_internal;

constexpr int32_t kCardW = 1003;
constexpr int32_t kCardH = 550;
constexpr uint32_t kOptionIdCount = 89;
constexpr int32_t kOptionValueMin = -32768;
constexpr int32_t kOptionValueMax = 32767;
constexpr int32_t kJewelValueMax = 2047;
int32_t g_deltas[] = {1, 10, 100, 1000, -1, -10, -100, -1000};
uint32_t g_group_indices[] = {0, 1, 2};
std::vector<int32_t> g_group_ids[3];

struct OptionRowContext {
    int32_t id = 0;
};

QolUiHandle g_root = nullptr;
QolUiHandle g_columns[3]{};
QolUiHandle g_footer = nullptr;
QolUiHandle g_confirm = nullptr;
QolUiHandle g_cancel = nullptr;
QolUiHandle g_adjust[8]{};
OptionRowContext g_rows[kOptionIdCount]{};
bool g_open_requested = false;
uint32_t g_section = 0;
uint32_t g_index = 0;
int32_t g_id = 0;
int32_t g_value = 0;
bool g_jewel_self = false;
int64_t g_sync_offset = 0;
char g_error[64]{};
enum class Action : uint8_t { kNone, kConfirm, kCancel };
Action g_pending = Action::kNone;

void noop_draw(void*) {}
void row_clicked(void* user) {
    const auto* row = static_cast<const OptionRowContext*>(user);
    if (row != nullptr) {
        g_id = row->id;
        g_error[0] = '\0';
    }
}
void adjust_clicked(void* user) {
    const auto* delta = static_cast<const int32_t*>(user);
    if (delta == nullptr) return;
    const int32_t minimum = g_jewel_self ? 0 : kOptionValueMin;
    const int32_t maximum = g_jewel_self ? kJewelValueMax : kOptionValueMax;
    g_value = static_cast<int32_t>(std::clamp<int64_t>(
        static_cast<int64_t>(g_value) + *delta, minimum, maximum));
}
void confirm_clicked(void*) { g_pending = Action::kConfirm; }
void cancel_clicked(void*) { g_pending = Action::kCancel; }

uint32_t group_first(uint32_t group) {
    return group == 0 ? 0u : group == 1 ? 17u : 37u;
}
void build_group_ids() {
    for (auto& ids : g_group_ids) ids.clear();
    for (uint32_t group = 0; group < 3; ++group) {
        const uint32_t first = group_first(group);
        // 基本 0..16，进阶 17..36，特殊 37..（C18 到 63，其他变体到 88）。
        const uint32_t count = group == 0 ? 17u : group == 1 ? 20u : g_jewel_self ? 27u : 52u;
        for (uint32_t row = 0; row < count; ++row) {
            const int32_t id = static_cast<int32_t>(first + row);
            const char* name = static_text(QolItemStaticTextKind::kOptionName, id);
            if (name != nullptr && name[0] != '\0') g_group_ids[group].push_back(id);
        }
    }
}
void draw_option_row(void* user, uint32_t row_index, QolUiHandle row, uint8_t) {
    const auto* adapter = ui();
    if (adapter == nullptr || user == nullptr) return;
    const uint32_t group = *static_cast<const uint32_t*>(user);
    if (group >= 3 || row_index >= g_group_ids[group].size()) return;
    const int32_t id = g_group_ids[group][row_index];
    const char* name = static_text(QolItemStaticTextKind::kOptionName, id);
    char label[160]{};
    if (name == nullptr || name[0] == '\0')
        std::snprintf(label, sizeof(label), "ID %d", id);
    else
        std::snprintf(label, sizeof(label), "ID %d  %s", id, name);
    int64_t abs_x = 0, abs_y = 0;
    if (adapter->control_absolute_origin != nullptr &&
        succeeded(adapter->control_absolute_origin(row, &abs_x, &abs_y)) &&
        adapter->draw_help_option_row != nullptr) {
        int32_t pressed = 0;
        if (adapter->control_pressed != nullptr)
            (void)adapter->control_pressed(row, &pressed);
        (void)adapter->draw_help_option_row({abs_x + 2, abs_y + 1, 214, 42},
            static_cast<uint8_t>(id == g_id), static_cast<uint8_t>(pressed != 0));
    }
    draw_text(row, label, 8, 14, id == g_id ? kGold : kText, 0, 0);
}

void draw_card_button(QolUiHandle button, const char* label,
                      int32_t width, int32_t height) {
    const auto* adapter = ui();
    if (adapter == nullptr || button == nullptr) return;
    int64_t x = 0, y = 0;
    if (adapter->control_absolute_origin == nullptr ||
        !succeeded(adapter->control_absolute_origin(button, &x, &y))) return;
    int32_t pressed = 0;
    if (adapter->control_pressed != nullptr)
        (void)adapter->control_pressed(button, &pressed);
    if (adapter->draw_help_option_row != nullptr)
        (void)adapter->draw_help_option_row({x, y, width, height}, 0,
            static_cast<uint8_t>(pressed != 0));
    draw_center_text(button, label, width / 2, height / 2 - 8, kText);
}

void selected_name(char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) return;
    const char* name = static_text(QolItemStaticTextKind::kOptionName, g_id);
    if (name == nullptr || name[0] == '\0')
        std::snprintf(out, capacity, "ID %d", g_id);
    else
        std::snprintf(out, capacity, "%s", name);
}

bool valid_target() {
    if (g_section == 0) return g_index < g_draft.native_option_count;
    if (g_section == 2) return g_index < g_draft.enchant_option_count;
    if (g_section != 1) return false;
    if (g_draft.class_index == kItemClassGem) return true;
    return g_index < g_draft.jewel_option_count;
}

void write_option(QolItemInventoryOptionView& option) {
    option.id = g_id;
    option.value = static_cast<int16_t>(g_value);
}

bool native_option_id_exists(int32_t id) {
    if (g_section != 0 || id < 0) return false;
    for (uint32_t index = 0; index < g_draft.native_option_count; ++index) {
        if (index == g_index) continue;
        if (g_draft.native_options[index].id == id) return true;
    }
    return false;
}
}  // namespace

void option_card_open(uint32_t section, uint32_t index) {
    if (section > 2) return;
    g_section = section;
    g_index = index;
    g_error[0] = '\0';
    g_open_requested = true;
}
bool option_card_open_requested() { return g_open_requested; }
void option_card_clear_open_request() { g_open_requested = false; }
bool option_card_active() { return g_root != nullptr; }

bool option_card_handle_event(uint64_t event, uint64_t param, uint64_t param2,
                              uint32_t* consumed) {
    const auto* c = components();
    if (g_root == nullptr || c == nullptr || c->dispatch_native_event == nullptr)
        return false;
    uint32_t local = 0;
    if (g_footer != nullptr) {
        (void)c->dispatch_native_event(g_footer, event, param, param2, &local);
        if (local != 0) {
            if (consumed != nullptr) *consumed = local;
            return true;
        }
    }
    (void)c->dispatch_native_event(g_root, event, param, param2, &local);
    if (consumed != nullptr) *consumed = local;
    return true;
}

void destroy_option_card() {
    const auto* c = components();
    if (c != nullptr && c->destroy != nullptr) {
        for (auto& column : g_columns) {
            if (column != nullptr) (void)c->destroy(column);
            column = nullptr;
        }
        for (auto& button : g_adjust) {
            if (button != nullptr) (void)c->destroy(button);
            button = nullptr;
        }
        if (g_confirm != nullptr) (void)c->destroy(g_confirm);
        if (g_cancel != nullptr) (void)c->destroy(g_cancel);
        if (g_footer != nullptr) (void)c->destroy(g_footer);
        if (g_root != nullptr) (void)c->destroy(g_root);
    }
    g_confirm = nullptr;
    g_cancel = nullptr;
    g_footer = nullptr;
    g_root = nullptr;
    g_open_requested = false;
    g_pending = Action::kNone;
    g_sync_offset = 0;
    for (auto& ids : g_group_ids) ids.clear();
}

bool create_option_card() {
    const auto* c = components();
    const auto* adapter = ui();
    if (c == nullptr || adapter == nullptr || c->panel_create == nullptr ||
        c->scroll_list_create == nullptr || c->scroll_row_button_create == nullptr ||
        c->button_create == nullptr || c->scroll_list_set_selection_enabled == nullptr ||
        adapter->set_draw_proc == nullptr) return false;
    if (!valid_target()) return false;
    g_jewel_self = g_section == 1 && g_draft.class_index == kItemClassGem;
    const QolItemInventoryOptionView* option = nullptr;
    if (g_jewel_self) {
        g_id = std::clamp(g_draft.jewel_attribute_type, 0, 63);
        g_value = std::clamp(g_draft.jewel_value, 0, kJewelValueMax);
    } else {
        option = g_section == 0 ? &g_draft.native_options[g_index] :
            g_section == 1 ? &g_draft.jewel_options[g_index] :
                             &g_draft.enchant_options[g_index];
        g_id = std::clamp(option->id, 0, 88);
        g_value = std::clamp<int32_t>(option->value, kOptionValueMin, kOptionValueMax);
    }

    const UiRect panel = panel_rect();
    const UiRect card_rect{panel.x + (panel.w - kCardW) / 2,
        panel.y + (panel.h - kCardH) / 2, kCardW, kCardH};
    const UiPanelDesc card{card_rect, {0, 0, kCardW, kCardH}, 0, 0};
    if (!succeeded(c->panel_create(&card, nullptr, &g_root))) return false;

    constexpr int32_t col_x[] = {16, 242, 468};
    build_group_ids();
    uint32_t rows = 0;
    for (const auto& ids : g_group_ids)
        rows = std::max<uint32_t>(rows, static_cast<uint32_t>(ids.size()));
    if (rows == 0) rows = 1;
    g_sync_offset = 0;
    for (uint32_t group = 0; group < 3; ++group) {
        const auto& ids = g_group_ids[group];
        const uint32_t count = static_cast<uint32_t>(ids.size());
        const int32_t selected = static_cast<int32_t>(
            std::find(ids.begin(), ids.end(), g_id) - ids.begin());
        const int32_t selected_row = selected < static_cast<int32_t>(count) ? selected : -1;
        const UiScrollListDesc list{{col_x[group], 54, 220, 420}, 44,
            rows, selected_row, &draw_option_row, nullptr, &g_group_indices[group]};
        if (!succeeded(c->scroll_list_create(&list, g_root, &g_columns[group])))
            return false;
        if (!succeeded(c->scroll_list_set_selection_enabled(g_columns[group], 0)))
            return false;
        if (selected_row >= 0) {
            constexpr int32_t visible_rows = 9;
            const int64_t offset = selected_row > visible_rows
                ? static_cast<int64_t>(selected_row - visible_rows) * 44 : 0;
            g_sync_offset = std::max(g_sync_offset, offset);
        }
        for (uint32_t row = 0; row < count; ++row) {
            const int32_t id = ids[row];
            g_rows[id].id = id;
            QolUiHandle button = nullptr;
            if (!succeeded(c->scroll_row_button_create(g_columns[group], row,
                    {2, 1, 214, 42}, &row_clicked, &g_rows[id], &button)))
                return false;
            if (!succeeded(adapter->set_draw_proc(button, &noop_draw))) {
                if (c->destroy != nullptr) (void)c->destroy(button);
                return false;
            }
        }
    }
    if (g_sync_offset > 0 && c->scroll_set_offset != nullptr) {
        for (QolUiHandle column : g_columns) {
            if (column != nullptr) (void)c->scroll_set_offset(column, g_sync_offset);
        }
    }

    for (uint32_t index = 0; index < 8; ++index) {
        const int32_t col = static_cast<int32_t>(index % 2);
        const int32_t row = static_cast<int32_t>(index / 2);
        const UiButtonDesc button{{720 + col * 120, 284 + row * 46, 104, 44},
            "", 0, &adjust_clicked, &g_deltas[index]};
        if (!succeeded(c->button_create(&button, g_root, &g_adjust[index])) ||
            !succeeded(adapter->set_draw_proc(g_adjust[index], &noop_draw)))
            return false;
    }

    const UiPanelDesc footer_desc{{panel.x + (panel.w - kCardW) / 2,
        panel.y + (panel.h - kCardH) / 2 + 488, kCardW, 48}, {0, 0, kCardW, 48}, 0, 0};
    if (!succeeded(c->panel_create(&footer_desc, nullptr, &g_footer))) return false;
    const UiButtonDesc confirm{{352, 0, 140, 48}, "", 0, &confirm_clicked, nullptr};
    if (!succeeded(c->button_create(&confirm, g_footer, &g_confirm)) ||
        !succeeded(adapter->set_draw_proc(g_confirm, &noop_draw))) return false;
    const UiButtonDesc cancel{{512, 0, 140, 48}, "", 0, &cancel_clicked, nullptr};
    if (!succeeded(c->button_create(&cancel, g_footer, &g_cancel)) ||
        !succeeded(adapter->set_draw_proc(g_cancel, &noop_draw))) return false;
    return true;
}

void draw_option_card() {
    if (g_root == nullptr) return;
    const auto* adapter = ui();
    const auto* c = components();
    if (adapter == nullptr || c == nullptr) return;
    const UiRect panel = panel_rect();
    const UiRect card{panel.x + (panel.w - kCardW) / 2,
        panel.y + (panel.h - kCardH) / 2, kCardW, kCardH};
    if (adapter->fill_rect_alpha != nullptr)
        (void)adapter->fill_rect_alpha({panel.x, panel.y, panel.w, panel.h}, 0x0000, 40);
    if (c->draw_container_styled != nullptr) {
        const UiContainerStyleDesc style{card, 0x0000, 60, 1};
        (void)c->draw_container_styled(&style);
    }
    int64_t offsets[3] = {g_sync_offset, g_sync_offset, g_sync_offset};
    for (uint32_t group = 0; group < 3; ++group) {
        if (g_columns[group] != nullptr) {
            (void)c->scroll_list_process(g_columns[group]);
            if (c->scroll_list_get_offset != nullptr)
                (void)c->scroll_list_get_offset(g_columns[group], &offsets[group]);
        }
    }
    int64_t target = g_sync_offset;
    bool moved = false;
    for (int64_t offset : offsets) {
        if (offset != g_sync_offset) {
            target = offset;
            moved = true;
        }
    }
    if (moved) {
        g_sync_offset = target;
        if (c->scroll_set_offset != nullptr) {
            for (uint32_t group = 0; group < 3; ++group) {
                if (g_columns[group] != nullptr && offsets[group] != target)
                    (void)c->scroll_set_offset(g_columns[group], target);
            }
        }
    }
    for (uint32_t group = 0; group < 3; ++group) {
        constexpr const char* group_names[] = {"基本", "进阶", "特殊"};
        constexpr int32_t col_x[] = {16, 242, 468};
        draw_text(g_root, group_names[group], col_x[group] + 110, 24, kGold, 2, 1);
        if (g_columns[group] != nullptr)
            (void)c->scroll_list_draw(g_columns[group]);
    }
    char name[128]{};
    char value[48]{};
    selected_name(name, sizeof(name));
    const char* source_title = g_section == 0 ? "装备原生词条" :
        g_section == 2 ? "附魔词条" :
        g_jewel_self ? "宝石本体词条" : "装备上的宝石词条";
    const char* percent = static_text(QolItemStaticTextKind::kItemOptionPercent, g_id);
    const bool is_percent = !g_jewel_self && percent != nullptr &&
        percent[0] == '1';
    if (is_percent)
        std::snprintf(value, sizeof(value), "%+.1f%%", static_cast<double>(g_value) / 10.0);
    else
        std::snprintf(value, sizeof(value), "%+d", g_value);
    draw_text(g_root, source_title, 720, 30, kGold, 0, 1);
    draw_text(g_root, name, 720, 72, kText, 0, 1);
    char id_text[32]{};
    std::snprintf(id_text, sizeof(id_text), "ID %d", g_id);
    draw_text(g_root, id_text, 720, 112, kMuted, 0, 0);
    draw_text(g_root, "当前数值", 720, 170, kMuted, 0, 0);
    draw_text(g_root, value, 720, 204, kGold, 0, 1);
    if (g_error[0] != '\0')
        draw_text(g_root, g_error, 720, 246, contracts::ui_theme::kError, 0, 0);
    for (uint32_t index = 0; index < 8; ++index) {
        char label[16]{};
        std::snprintf(label, sizeof(label), "%+d", g_deltas[index]);
        draw_card_button(g_adjust[index], label, 104, 44);
    }
    draw_card_button(g_confirm, "确认", 140, 48);
    draw_card_button(g_cancel, "取消", 140, 48);
}

void option_card_apply() {
    if (g_pending == Action::kNone) return;
    const Action action = g_pending;
    g_pending = Action::kNone;
    if (action == Action::kConfirm && valid_target()) {
        if (native_option_id_exists(g_id)) {
            std::snprintf(g_error, sizeof(g_error), "该属性已存在，不能重复添加");
            return;
        }
        if (g_jewel_self) {
            g_draft.jewel_attribute_type = std::clamp(g_id, 0, 63);
            g_draft.jewel_value = std::clamp(g_value, 0, kJewelValueMax);
            g_draft.jewel_attribute_type_valid = true;
        } else if (g_section == 0) {
            write_option(g_draft.native_options[g_index]);
        } else if (g_section == 1) {
            write_option(g_draft.jewel_options[g_index]);
        } else {
            write_option(g_draft.enchant_options[g_index]);
        }
        g_draft.dirty = true;
        rebuild_right_list();
    }
    destroy_option_card();
}
}  // namespace qol::features::item_editor
