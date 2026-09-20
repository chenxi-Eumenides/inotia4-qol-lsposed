#include "game/access/craft.h"

#include "data/native/game_symbols.h"
#include "game_access.h"

namespace qol::game {

namespace {

// filled[0..count-1] 中索引最小的未填格；全满或入参异常返回 -1。
// （原 feature/gemcraft/gemcraft_rules.h 的 first_empty_slot，随 gemcraft 归并入本层。）
int first_empty_slot(const bool* filled, int count) {
    if (filled == nullptr || count <= 0) return -1;
    for (int i = 0; i < count; ++i) {
        if (!filled[i]) return i;
    }
    return -1;
}

}  // namespace

void* craft_slot(size_t offset) {
    if (g_uimix == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + offset);
}

uint32_t craft_mix_type() {
    if (g_uimix == nullptr) return 0;
    return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_MIXTYPE);
}

int64_t craft_ui_type() {
    if (g_uimix == nullptr) return -1;
    return static_cast<int64_t>(
        *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_TYPE));
}

int64_t craft_get_ui_type() {
    return fn_uimix_get_type != nullptr ? static_cast<int64_t>(fn_uimix_get_type()) : -1;
}

int64_t craft_selected_recipe_at(int64_t type) {
    if (g_uimix == nullptr || type < 0) return 0;
    return *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) +
                                       UIMIX_SLOT_SELECTED_RECIPE_BASE +
                                       static_cast<size_t>(type) * UIMIX_SLOT_SELECTED_RECIPE_STRIDE);
}

void craft_set_selected_recipe_at(int64_t type, int64_t value) {
    if (g_uimix == nullptr || type < 0) return;
    *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) +
                                UIMIX_SLOT_SELECTED_RECIPE_BASE +
                                static_cast<size_t>(type) * UIMIX_SLOT_SELECTED_RECIPE_STRIDE) =
        value;
}

void* craft_target_slot_control() {
    if (g_uimix == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_TARGET_ITEM);
}

void* craft_target_slot_item() {
    void* ctrl = craft_target_slot_control();
    if (ctrl == nullptr || fn_control_item_get_item == nullptr) return nullptr;
    return fn_control_item_get_item(ctrl);
}

void craft_set_target_slot_item(void* item) {
    void* ctrl = craft_target_slot_control();
    if (ctrl == nullptr || fn_control_item_set_item == nullptr) return;
    fn_control_item_set_item(ctrl, item);
}

uint8_t craft_panel_state() {
    if (g_uimix == nullptr) return 0;
    return *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_STATE);
}

void* craft_stuff_slot_control(size_t index) {
    if (g_uimix == nullptr || fn_control_object_get_child == nullptr) return nullptr;
    void* group =
        *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_STUFF_GROUP);
    if (group == nullptr) return nullptr;
    return fn_control_object_get_child(group, static_cast<uint32_t>(index));
}

void* craft_stuff_item(size_t index) {
    void* ctrl = craft_stuff_slot_control(index);
    if (ctrl == nullptr || fn_control_item_get_item == nullptr) return nullptr;
    return fn_control_item_get_item(ctrl);
}

void craft_set_stuff_item(size_t index, void* item) {
    void* ctrl = craft_stuff_slot_control(index);
    if (ctrl == nullptr || fn_control_item_set_item == nullptr) return;
    fn_control_item_set_item(ctrl, item);
}

int craft_read_stuff_filled(bool out[kCraftStuffSlotCount]) {
    if (out == nullptr) return 0;
    int count = 0;
    for (size_t i = 0; i < kCraftStuffSlotCount; ++i) {
        out[i] = craft_stuff_item(i) != nullptr;
        if (out[i]) ++count;
    }
    return count;
}

int craft_read_stuff_categories(uint16_t out[kCraftStuffSlotCount]) {
    if (out == nullptr) return 0;
    int count = 0;
    for (size_t i = 0; i < kCraftStuffSlotCount; ++i) {
        out[i] = 0;  // 空格 = 0（匹配键的「空」）
        void* item = craft_stuff_item(i);
        if (item == nullptr) continue;
        const uint16_t type_flags =
            *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(item) + I_TYPE);
        out[i] = static_cast<uint16_t>((type_flags >> I_TYPE_CATEGORY_SHIFT) &
                                       I_TYPE_CATEGORY_MASK);
        ++count;
    }
    return count;
}

int64_t craft_selected_stuff_index() {
    if (g_uimix == nullptr) return -1;
    return *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_SELECTED_STUFF);
}

void craft_select_stuff_slot(int64_t index) {
    if (g_uimix == nullptr) return;
    *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_SELECTED_STUFF) = index;
}

int craft_select_first_empty_stuff_slot() {
    if (g_uimix == nullptr) return -1;
    bool filled[kCraftStuffSlotCount] = {false, false, false};
    craft_read_stuff_filled(filled);
    const int index = first_empty_slot(filled, static_cast<int>(kCraftStuffSlotCount));
    craft_select_stuff_slot(index);
    return index;
}

uint32_t craft_stuff_slot_count() {
    if (g_uimix == nullptr) return 0;
    return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_STUFF_NUM);
}

void craft_write_stuff_need(size_t index, uint16_t need) {
    if (g_uimix == nullptr) return;
    auto* stuff_list =
        *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_STUFF_LIST);
    if (stuff_list == nullptr) return;
    if (index >= craft_stuff_slot_count()) return;
    *reinterpret_cast<uint16_t*>(stuff_list + index * UIMIX_STUFF_ENTRY_SIZE +
                                 UIMIX_STUFF_ENTRY_NEED) = need;
}

void craft_set_cost_zero() {
    if (g_uimix == nullptr) return;
    *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_COST) = 0;
}

void* craft_selected_inven_item() {
    void* group = craft_slot(UIMIX_SLOT_ITEM_GROUP);
    if (group == nullptr || fn_control_object_get_cursor == nullptr ||
        fn_control_object_get_data == nullptr) {
        return nullptr;
    }
    void* cursor = fn_control_object_get_cursor(group);
    if (cursor == nullptr) return nullptr;
    void* data = fn_control_object_get_data(cursor);
    if (data == nullptr) return nullptr;
    return *reinterpret_cast<void**>(data);
}

void craft_init_mixing_state() {
    if (fn_uimix_init_mixing_state != nullptr) fn_uimix_init_mixing_state();
}

void craft_reset_stuff_item_control() {
    if (fn_uimix_reset_stuff_item_control != nullptr) fn_uimix_reset_stuff_item_control();
}

void craft_refresh_inven_items() {
    if (fn_ui_mix_refresh_inven_item != nullptr) fn_ui_mix_refresh_inven_item();
}

void craft_set_ui_type(int64_t type) {
    // type >= 5 禁止：原版 ResetActiveControl 对 type>=5 走未初始化寄存器路径。
    // 越界静默拒绝（game 层不依赖 infra 日志）。
    if (type < 0 || type >= UIMIX_RECIPE_TYPE_COUNT) return;
    if (fn_uimix_set_type == nullptr) return;
    fn_uimix_set_type(type);
}

void craft_play_sound(int16_t sound_id) {
    if (fn_sound_system_play != nullptr) fn_sound_system_play(sound_id);
}

void craft_show_text(uint32_t word_id) {
    if (fn_popup_create_ok_from_textdata != nullptr) {
        fn_popup_create_ok_from_textdata(word_id, 0, 0, 0);
    }
}

void craft_desc_set_off() {
    if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();
}

bool craft_show_yesno_from_textdata(uint32_t text_id, void* ok, void* cancel) {
    if (fn_popup_create_yesno_from_textdata == nullptr) return false;
    fn_popup_create_yesno_from_textdata(text_id, 0, 0, ok, cancel, nullptr);
    return true;
}

int craft_character_level() {
    if (fn_get_member == nullptr) return 0;
    void* member = fn_get_member(0);  // 0 = 主角（PARTY_GetMember 下标）
    if (member == nullptr) return 0;
    return static_cast<int>(
        *reinterpret_cast<int8_t*>(static_cast<uint8_t*>(member) + C_LEVEL));
}

int craft_category_held_count(uint16_t category) {
    if (fn_inven_get_item_count == nullptr) return 0;
    const int n = fn_inven_get_item_count(static_cast<int32_t>(category));
    return n > 0 ? n : 0;
}

int craft_math_get_random(int lo, int hi) {
    if (fn_math_get_random == nullptr) return lo;
    return fn_math_get_random(lo, hi);
}

int32_t craft_is_apply_stuff(void* target, void* material) {
    if (fn_ui_equip_is_apply_stuff == nullptr) return -1;
    return fn_ui_equip_is_apply_stuff(target, material) != 0 ? 1 : 0;
}

}  // namespace qol::game
