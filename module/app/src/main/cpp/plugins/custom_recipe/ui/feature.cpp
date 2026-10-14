#include "feature.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../../../contracts/plugin.h"
#include "../../../contracts/ui.h"

namespace qol::features::custom_recipe::ui {

namespace {

std::atomic<int> g_verify_log_budget{16};
const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::CraftFactsApi* g_craft_facts = nullptr;
const contracts::QolCraftUiWritePort* g_write_port = nullptr;
thread_local int32_t t_current_text_scope = 0;

}  // namespace

void set_context(const contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
}

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

void set_item_facts(const contracts::ItemFactsApi* facts) { g_item_facts = facts; }

void set_craft_facts(const contracts::CraftFactsApi* facts) { g_craft_facts = facts; }

void set_write_port(const contracts::QolCraftUiWritePort* port) { g_write_port = port; }

const contracts::ItemFactsApi* item_facts() { return g_item_facts; }

void log_message(int32_t level, const char* format, ...) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr || format == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length <= 0) return;
    const uint32_t size = static_cast<uint32_t>(length) >= contracts::kMessageCap
                              ? contracts::kMessageCap
                              : static_cast<uint32_t>(length);
    (void)g_sdk->log.write(level, 15, "plugins/custom_recipe/ui", message, size);
}

uint32_t craft_mix_type() {
    uint32_t value = 0;
    if (g_craft_facts != nullptr && g_craft_facts->read.mix_type != nullptr) {
        (void)g_craft_facts->read.mix_type(&value);
    }
    return value;
}

int64_t craft_ui_type() {
    uint32_t value = 0;
    if (g_craft_facts == nullptr || g_craft_facts->read.ui_type == nullptr ||
        g_craft_facts->read.ui_type(&value).ok != kQolOk) {
        return -1;
    }
    return static_cast<int64_t>(value);
}

int64_t craft_selected_recipe_at(int64_t type) {
    int64_t value = -1;
    if (g_craft_facts != nullptr && g_craft_facts->read_only.selected_recipe != nullptr &&
        type >= 0 && type <= 4) {
        (void)g_craft_facts->read_only.selected_recipe(static_cast<int32_t>(type), &value);
    }
    return value;
}

void craft_set_selected_recipe_at(int64_t type, int64_t value) {
    if (g_write_port != nullptr && g_write_port->set_selected_recipe != nullptr && type >= 0 &&
        type <= 4) {
        (void)g_write_port->set_selected_recipe(static_cast<int32_t>(type), value);
    }
}

int64_t craft_selected_stuff_index() {
    int64_t value = -1;
    if (g_craft_facts != nullptr && g_craft_facts->read.selected_stuff_index != nullptr) {
        (void)g_craft_facts->read.selected_stuff_index(&value);
    }
    return value;
}

contracts::QolItemHandle craft_selected_inventory_item() {
    contracts::QolItemHandle item = contracts::kInvalidItemHandle;
    if (g_craft_facts != nullptr && g_craft_facts->read_only.selected_inventory_item != nullptr) {
        (void)g_craft_facts->read_only.selected_inventory_item(&item);
    }
    return item;
}

contracts::QolItemHandle craft_target_slot_item() {
    contracts::QolItemHandle item = contracts::kInvalidItemHandle;
    if (g_craft_facts != nullptr && g_craft_facts->read_only.target_slot_item != nullptr) {
        (void)g_craft_facts->read_only.target_slot_item(&item);
    }
    return item;
}

void craft_set_target_slot_item(contracts::QolItemHandle item) {
    if (g_write_port != nullptr && g_write_port->set_target_slot_item != nullptr) {
        (void)g_write_port->set_target_slot_item(item);
    }
}

void craft_set_cost_zero() {
    if (g_write_port != nullptr && g_write_port->set_cost_zero != nullptr) {
        (void)g_write_port->set_cost_zero();
    }
}

contracts::QolItemHandle craft_stuff_item(size_t index) {
    contracts::QolItemHandle item = contracts::kInvalidItemHandle;
    if (g_craft_facts != nullptr && g_craft_facts->read.stuff_item != nullptr &&
        index < kStuffSlotCount) {
        (void)g_craft_facts->read.stuff_item(static_cast<uint32_t>(index), &item);
    }
    return item;
}

bool craft_stuff_slot_available(size_t index) {
    int32_t available = 0;
    if (g_craft_facts != nullptr && g_craft_facts->read_only.stuff_slot_available != nullptr &&
        index < kStuffSlotCount) {
        return g_craft_facts->read_only.stuff_slot_available(
                   static_cast<uint32_t>(index), &available).ok == kQolOk && available != 0;
    }
    return false;
}

int craft_category_held_count(uint16_t category) {
    int32_t value = 0;
    if (g_craft_facts != nullptr && g_craft_facts->read.category_held_count != nullptr &&
        g_craft_facts->read.category_held_count(category, &value).ok == kQolOk) {
        return value;
    }
    return 0;
}

int craft_character_level() {
    int32_t value = 0;
    if (g_craft_facts != nullptr && g_craft_facts->read.character_level != nullptr &&
        g_craft_facts->read.character_level(&value).ok == kQolOk) {
        return value;
    }
    return 0;
}

void craft_read_stuff_categories(uint16_t out[kStuffSlotCount]) {
    if (out == nullptr) return;
    for (size_t i = 0; i < kStuffSlotCount; ++i) {
        out[i] = 0;
        const contracts::QolItemHandle handle = craft_stuff_item(i);
        int32_t category = 0;
        if (handle.slot != 0 && handle.generation != 0 && g_item_facts != nullptr &&
            g_item_facts->category != nullptr &&
            g_item_facts->category(handle, &category).ok == kQolOk && category >= 0) {
            out[i] = static_cast<uint16_t>(category);
        }
    }
}

void craft_write_stuff_need(size_t index, uint16_t category, uint16_t count) {
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) return;
    contracts::CraftWriteStuffNeedPayload payload{
        static_cast<uint32_t>(index), static_cast<int32_t>(category), static_cast<int32_t>(count)};
    contracts::QolCommandTicket ticket = 0;
    (void)g_sdk->commands.post_typed(contracts::kCraftWriteStuffNeedHandlerId, &payload,
                                     sizeof(payload), &ticket);
}

void craft_set_stuff_item(size_t index, contracts::QolItemHandle item) {
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) return;
    contracts::CraftSetStuffItemPayload payload{static_cast<uint32_t>(index), item};
    contracts::QolCommandTicket ticket = 0;
    (void)g_sdk->commands.post_typed(contracts::kCraftSetStuffItemHandlerId, &payload,
                                     sizeof(payload), &ticket);
}

void craft_select_first_empty_stuff_slot() {
    if (g_write_port != nullptr && g_write_port->select_first_empty_stuff_slot != nullptr) {
        int32_t ignored = -1;
        (void)g_write_port->select_first_empty_stuff_slot(&ignored);
    }
}

void craft_reset_stuff_item_control() {
    if (g_write_port != nullptr && g_write_port->reset_stuff_item_control != nullptr) {
        (void)g_write_port->reset_stuff_item_control();
    }
}

void craft_refresh_inventory_items() {
    if (g_write_port != nullptr && g_write_port->refresh_inventory_items != nullptr) {
        (void)g_write_port->refresh_inventory_items();
    }
}

void craft_init_mixing_state() {
    if (g_write_port != nullptr && g_write_port->init_mixing_state != nullptr) {
        (void)g_write_port->init_mixing_state();
    }
}

void craft_set_ui_type(int64_t type) {
    if (g_write_port != nullptr && g_write_port->set_ui_type != nullptr) {
        (void)g_write_port->set_ui_type(type);
    }
}

void craft_play_sound(int16_t sound_id) {
    if (g_write_port != nullptr && g_write_port->play_sound != nullptr) {
        (void)g_write_port->play_sound(sound_id);
    }
}

void craft_show_text(uint32_t text_id) {
    if (g_write_port != nullptr && g_write_port->show_text != nullptr) {
        (void)g_write_port->show_text(text_id);
    }
}

bool craft_show_yesno(uint32_t text_id, contracts::QolCraftPopupCallback ok,
                      contracts::QolCraftPopupCallback cancel) {
    if (g_write_port == nullptr || g_write_port->show_yesno_from_text == nullptr) return false;
    return g_write_port->show_yesno_from_text(text_id, ok, cancel).ok == kQolOk;
}

int craft_panel_state() {
    int32_t value = 0;
    if (g_craft_facts != nullptr && g_craft_facts->read.panel_state != nullptr) {
        (void)g_craft_facts->read.panel_state(&value);
    }
    return value;
}

TextScopeGuard::TextScopeGuard(int32_t scope)
    : sdk_(g_sdk), previous_scope_(t_current_text_scope), pushed_(false) {
    if (sdk_ == nullptr || sdk_->text_scope.push == nullptr || sdk_->text_scope.pop == nullptr) {
        return;
    }
    const int32_t host_scope = scope >= 0 && scope <= 4 ? scope : 0;
    pushed_ = sdk_->text_scope.push(host_scope).ok == kQolOk;
    if (pushed_) t_current_text_scope = scope;
}

TextScopeGuard::~TextScopeGuard() {
    if (pushed_ && sdk_ != nullptr && sdk_->text_scope.pop != nullptr) {
        (void)sdk_->text_scope.pop();
    }
    t_current_text_scope = previous_scope_;
}

bool log_budget_take() {
    return g_verify_log_budget.fetch_sub(1) > 0;
}

void finish_three_slot_craft() {
    craft_init_mixing_state();
    craft_reset_stuff_item_control();
    craft_refresh_inventory_items();
    craft_play_sound(kSoundCraftSuccess);
    craft_show_text(kTextCraftSuccess);
}

void select_on_view_enter() {
    if (craft_ui_type() != 1) return;
    if (craft_panel_state() == 0) return;
    craft_select_first_empty_stuff_slot();
}

void reset_stuff_and_refresh() {
    // 清空填入格 → 刷新背包网格。
    // `UIMix_ResetStuffItemControl` 已被本模块以函数入口 hook 接管（补「清空后选中第一个
    // 空格」，见 reset_stuff_execute），所以这里调完就会自动恢复选中落点。
    craft_reset_stuff_item_control();
    craft_refresh_inventory_items();
}

bool is_stuff_ctrl(void* ctrl) {
    if (ctrl == nullptr || g_sdk == nullptr || g_sdk->ui == nullptr) return false;
    constexpr size_t required_size = offsetof(contracts::QolUiAdapter, craft_stuff_slot_index) +
                                     sizeof(contracts::QolUiAdapter::craft_stuff_slot_index);
    if (g_sdk->ui->size < required_size || g_sdk->ui->major != contracts::kUiAdapterMajor ||
        g_sdk->ui->minor < contracts::kUiAdapterMinor ||
        g_sdk->ui->craft_stuff_slot_index == nullptr) return false;
    int32_t slot = -1;
    return g_sdk->ui->craft_stuff_slot_index(ctrl, &slot).ok == kQolOk &&
           slot >= 0 && slot < static_cast<int32_t>(kStuffSlotCount);
}

int32_t current_text_scope() { return t_current_text_scope; }

}  // namespace qol::features::custom_recipe::ui
