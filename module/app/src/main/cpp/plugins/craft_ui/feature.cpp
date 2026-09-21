#include "feature.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../../contracts/plugin.h"

namespace qol::features::craft_ui {

namespace {

std::atomic<int> g_verify_log_budget{16};
const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;

// 填入格控件缓存（绘制层只做指针比较，见 feature.h 注释）。
void* g_stuff_ctrl[kStuffSlotCount] = {nullptr, nullptr, nullptr};

}  // namespace

void set_context(const contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
}

const contracts::QolHostSdk* host_sdk() { return g_sdk; }
const contracts::QolGameHost* game_host() { return g_game; }

void set_item_facts(const contracts::ItemFactsApi* facts) { g_item_facts = facts; }

const contracts::ItemFactsApi* item_facts() { return g_item_facts; }

bool config_enabled(const char* key) {
    int32_t value = 0;
    if (g_sdk == nullptr || g_sdk->config.get_bool == nullptr || key == nullptr) return false;
    (void)g_sdk->config.get_bool(key, 0, &value);
    return value != 0;
}

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
    (void)g_sdk->log.write(level, 15, "plugins/craft_ui", message, size);
}

uint32_t craft_mix_type() {
    uint32_t value = 0;
    if (g_game != nullptr && g_game->read.craft.mix_type != nullptr) {
        (void)g_game->read.craft.mix_type(&value);
    }
    return value;
}

int64_t craft_ui_type() {
    uint32_t value = 0;
    if (g_game == nullptr || g_game->read.craft.ui_type == nullptr ||
        g_game->read.craft.ui_type(&value).ok != kQolOk) {
        return -1;
    }
    return static_cast<int64_t>(value);
}

int64_t craft_selected_recipe_at(int64_t type) {
    int64_t value = -1;
    if (g_game != nullptr && g_game->read_only.craft.selected_recipe != nullptr && type >= 0 && type <= 4) {
        (void)g_game->read_only.craft.selected_recipe(static_cast<int32_t>(type), &value);
    }
    return value;
}

void craft_set_selected_recipe_at(int64_t type, int64_t value) {
    if (g_game != nullptr && g_game->craft_action.set_selected_recipe != nullptr && type >= 0 && type <= 4) {
        (void)g_game->craft_action.set_selected_recipe(static_cast<int32_t>(type), value);
    }
}

int64_t craft_selected_stuff_index() {
    int64_t value = -1;
    if (g_game != nullptr && g_game->read.craft.selected_stuff_index != nullptr) {
        (void)g_game->read.craft.selected_stuff_index(&value);
    }
    return value;
}

void* craft_selected_inventory_item() {
    void* item = nullptr;
    if (g_game != nullptr && g_game->read_only.craft.selected_inventory_item != nullptr) {
        (void)g_game->read_only.craft.selected_inventory_item(&item);
    }
    return item;
}

void* craft_target_slot_item() {
    void* item = nullptr;
    if (g_game != nullptr && g_game->read_only.craft.target_slot_item != nullptr) {
        (void)g_game->read_only.craft.target_slot_item(&item);
    }
    return item;
}

void craft_set_target_slot_item(void* item) {
    if (g_game != nullptr && g_game->write.craft.set_target_slot_item != nullptr) {
        (void)g_game->write.craft.set_target_slot_item(item);
    }
}

void* craft_stuff_item(size_t index) {
    void* item = nullptr;
    if (g_game != nullptr && g_game->read.craft.stuff_item != nullptr && index < kStuffSlotCount) {
        (void)g_game->read.craft.stuff_item(static_cast<uint32_t>(index), &item);
    }
    return item;
}

void* craft_stuff_slot_control(size_t index) {
    void* control = nullptr;
    if (g_game != nullptr && g_game->read_only.craft.stuff_slot_control != nullptr && index < kStuffSlotCount) {
        (void)g_game->read_only.craft.stuff_slot_control(static_cast<uint32_t>(index), &control);
    }
    return control;
}

int craft_category_held_count(uint16_t category) {
    int32_t value = 0;
    if (g_game != nullptr && g_game->read.craft.category_held_count != nullptr &&
        g_game->read.craft.category_held_count(category, &value).ok == kQolOk) {
        return value;
    }
    return 0;
}

int craft_character_level() {
    int32_t value = 0;
    if (g_game != nullptr && g_game->read.craft.character_level != nullptr &&
        g_game->read.craft.character_level(&value).ok == kQolOk) {
        return value;
    }
    return 0;
}

void craft_read_stuff_categories(uint16_t out[kStuffSlotCount]) {
    if (out == nullptr) return;
    for (size_t i = 0; i < kStuffSlotCount; ++i) {
        out[i] = 0;
        void* item = craft_stuff_item(i);
        int32_t category = 0;
        if (item != nullptr && g_item_facts != nullptr && g_item_facts->category != nullptr &&
            g_item_facts->category(item, &category).ok == kQolOk && category >= 0) {
            out[i] = static_cast<uint16_t>(category);
        }
    }
}

void craft_write_stuff_need(size_t index, uint16_t category, uint16_t count) {
    if (g_game != nullptr && g_game->write.craft.write_stuff_need != nullptr) {
        (void)g_game->write.craft.write_stuff_need(static_cast<uint32_t>(index), category, count);
    }
}

void craft_set_stuff_item(size_t index, void* item) {
    if (g_game != nullptr && g_game->write.craft.set_stuff_item != nullptr) {
        (void)g_game->write.craft.set_stuff_item(static_cast<uint32_t>(index), item);
    }
}

void craft_select_first_empty_stuff_slot() {
    if (g_game != nullptr && g_game->craft_action.select_first_empty_stuff_slot != nullptr) {
        int32_t ignored = -1;
        (void)g_game->craft_action.select_first_empty_stuff_slot(&ignored);
    }
}

void craft_reset_stuff_item_control() {
    if (g_game != nullptr && g_game->craft_action.reset_stuff_item_control != nullptr) {
        (void)g_game->craft_action.reset_stuff_item_control();
    }
}

void craft_refresh_inventory_items() {
    if (g_game != nullptr && g_game->craft_action.refresh_inventory_items != nullptr) {
        (void)g_game->craft_action.refresh_inventory_items();
    }
}

void craft_init_mixing_state() {
    if (g_game != nullptr && g_game->craft_action.init_mixing_state != nullptr) {
        (void)g_game->craft_action.init_mixing_state();
    }
}

void craft_set_ui_type(int64_t type) {
    if (g_game != nullptr && g_game->craft_action.set_ui_type != nullptr) {
        (void)g_game->craft_action.set_ui_type(type);
    }
}

void craft_play_sound(int16_t sound_id) {
    if (g_game != nullptr && g_game->craft_action.play_sound != nullptr) {
        (void)g_game->craft_action.play_sound(sound_id);
    }
}

void craft_show_text(uint32_t text_id) {
    if (g_game != nullptr && g_game->craft_action.show_text != nullptr) {
        (void)g_game->craft_action.show_text(text_id);
    }
}

bool craft_show_yesno(uint32_t text_id, void* ok, void* cancel) {
    if (g_game == nullptr || g_game->craft_action.show_yesno_from_text == nullptr) return false;
    return g_game->craft_action.show_yesno_from_text(text_id, ok, cancel).ok == kQolOk;
}

void craft_description_set_off() {
    if (g_game != nullptr && g_game->craft_action.description_set_off != nullptr) {
        (void)g_game->craft_action.description_set_off();
    }
}

int craft_panel_state() {
    int32_t value = 0;
    if (g_game != nullptr && g_game->read.craft.panel_state != nullptr) {
        (void)g_game->read.craft.panel_state(&value);
    }
    return value;
}

void craft_select_stuff_slot(int64_t index) {
    (void)index;
}

TextScopeGuard::TextScopeGuard(int32_t scope) : sdk_(g_sdk), pushed_(false) {
    if (sdk_ != nullptr && sdk_->text_scope.push != nullptr) {
        pushed_ = sdk_->text_scope.push(scope).ok == kQolOk;
    }
}

TextScopeGuard::~TextScopeGuard() {
    if (pushed_ && sdk_ != nullptr && sdk_->text_scope.pop != nullptr) {
        (void)sdk_->text_scope.pop();
    }
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

void refresh_stuff_ctrls() {
    for (size_t i = 0; i < kStuffSlotCount; ++i) {
        g_stuff_ctrl[i] = craft_stuff_slot_control(i);
    }
}

bool is_stuff_ctrl(void* ctrl) {
    if (ctrl == nullptr) return false;
    for (size_t i = 0; i < kStuffSlotCount; ++i) {
        if (g_stuff_ctrl[i] == ctrl) return true;
    }
    return false;
}

}  // namespace qol::features::craft_ui
