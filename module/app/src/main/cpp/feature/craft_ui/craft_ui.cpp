#include "feature/craft_ui/craft_ui.h"

#include "core/native/qol_log.h"
#include "game_access.h"
#include "game_symbols.h"

#include <cstddef>
#include <cstdint>

namespace craft_ui {

namespace {

// 日志域：界面层与配方层同属「合成器」这一个功能，统一用 kCustomRecipe 域，便于按域筛日志。
constexpr QolDomain kDomain = QolDomain::kCustomRecipe;

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

void* slot(size_t offset) {
    if (g_uimix == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + offset);
}

uint32_t mix_type() {
    if (g_uimix == nullptr) return 0;
    return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_MIXTYPE);
}

int64_t ui_type() {
    if (g_uimix == nullptr) return -1;
    return static_cast<int64_t>(
        *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_TYPE));
}

int64_t selected_recipe_at(int64_t type) {
    if (g_uimix == nullptr || type < 0) return 0;
    return *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) +
                                       UIMIX_SLOT_SELECTED_RECIPE_BASE +
                                       static_cast<size_t>(type) * UIMIX_SLOT_SELECTED_RECIPE_STRIDE);
}

void set_selected_recipe_at(int64_t type, int64_t value) {
    if (g_uimix == nullptr || type < 0) return;
    *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) +
                                UIMIX_SLOT_SELECTED_RECIPE_BASE +
                                static_cast<size_t>(type) * UIMIX_SLOT_SELECTED_RECIPE_STRIDE) =
        value;
}

void* target_slot_control() {
    if (g_uimix == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_TARGET_ITEM);
}

void* target_slot_item() {
    void* ctrl = target_slot_control();
    if (ctrl == nullptr || fn_control_item_get_item == nullptr) return nullptr;
    return fn_control_item_get_item(ctrl);
}

void set_target_slot_item(void* item) {
    void* ctrl = target_slot_control();
    if (ctrl == nullptr || fn_control_item_set_item == nullptr) return;
    fn_control_item_set_item(ctrl, item);
}

void* stuff_slot_control(size_t index) {
    if (g_uimix == nullptr || fn_control_object_get_child == nullptr) return nullptr;
    void* group =
        *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_STUFF_GROUP);
    if (group == nullptr) return nullptr;
    return fn_control_object_get_child(group, static_cast<uint32_t>(index));
}

void* stuff_item(size_t index) {
    void* ctrl = stuff_slot_control(index);
    if (ctrl == nullptr || fn_control_item_get_item == nullptr) return nullptr;
    return fn_control_item_get_item(ctrl);
}

void set_stuff_item(size_t index, void* item) {
    void* ctrl = stuff_slot_control(index);
    if (ctrl == nullptr || fn_control_item_set_item == nullptr) return;
    fn_control_item_set_item(ctrl, item);
}

int read_stuff_filled(bool out[kStuffSlotCount]) {
    if (out == nullptr) return 0;
    int count = 0;
    for (size_t i = 0; i < kStuffSlotCount; ++i) {
        out[i] = stuff_item(i) != nullptr;
        if (out[i]) ++count;
    }
    return count;
}

int read_stuff_categories(uint16_t out[kStuffSlotCount]) {
    if (out == nullptr) return 0;
    int count = 0;
    for (size_t i = 0; i < kStuffSlotCount; ++i) {
        out[i] = 0;  // 空格 = 0（匹配键的「空」）
        void* item = stuff_item(i);
        if (item == nullptr) continue;
        const uint16_t type_flags =
            *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(item) + I_TYPE);
        out[i] = static_cast<uint16_t>((type_flags >> I_TYPE_CATEGORY_SHIFT) &
                                       I_TYPE_CATEGORY_MASK);
        ++count;
    }
    return count;
}

int64_t selected_stuff_index() {
    if (g_uimix == nullptr) return -1;
    return *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_SELECTED_STUFF);
}

void select_stuff_slot(int64_t index) {
    if (g_uimix == nullptr) return;
    *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_SELECTED_STUFF) = index;
}

int select_first_empty_stuff_slot() {
    if (g_uimix == nullptr) return -1;
    bool filled[kStuffSlotCount] = {false, false, false};
    read_stuff_filled(filled);
    const int index = first_empty_slot(filled, static_cast<int>(kStuffSlotCount));
    select_stuff_slot(index);
    return index;
}

void* selected_inven_item() {
    void* group = slot(UIMIX_SLOT_ITEM_GROUP);
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

void init_mixing_state() {
    if (fn_uimix_init_mixing_state != nullptr) fn_uimix_init_mixing_state();
}

void reset_stuff_item_control() {
    if (fn_uimix_reset_stuff_item_control != nullptr) fn_uimix_reset_stuff_item_control();
}

void refresh_inven_items() {
    if (fn_ui_mix_refresh_inven_item != nullptr) fn_ui_mix_refresh_inven_item();
}

void reset_stuff_and_refresh() {
    // 清空填入格 → 刷新背包网格。
    // `UIMix_ResetStuffItemControl` 已被本模块以函数入口 hook 接管（补「清空后选中第一个
    // 空格」，见 craft_ui 的合成器挂钩安装），所以这里调完就会自动恢复选中落点。
    reset_stuff_item_control();
    refresh_inven_items();
}

void set_ui_type(int64_t type) {
    // type >= 5 禁止：原版 ResetActiveControl 对 type>=5 走未初始化寄存器路径。
    if (type < 0 || type >= UIMIX_RECIPE_TYPE_COUNT) {
        QOL_LOG_WARN(kDomain, "set_ui_type rejected type=%lld", static_cast<long long>(type));
        return;
    }
    if (fn_uimix_set_type == nullptr) return;
    fn_uimix_set_type(type);
}

void play_sound(int16_t sound_id) {
    if (fn_sound_system_play != nullptr) fn_sound_system_play(sound_id);
}

void show_text(uint32_t word_id) {
    if (fn_popup_create_ok_from_textdata != nullptr) {
        fn_popup_create_ok_from_textdata(word_id, 0, 0, 0);
    }
}

bool install_one(NativeHookFunType hook, uintptr_t target, void* replacement, void** backup,
                 const char* name) {
    const int rc = hook(reinterpret_cast<void*>(target), replacement, backup);
    if (rc != 0 || backup == nullptr || *backup == nullptr) {
        QOL_LOG_ERROR(kDomain, "%s hook failed rc=%d", name, rc);
        return false;
    }
    return true;
}

}  // namespace craft_ui
