#include "game/access/save.h"

#include "data/native/game_symbols.h"
#include "game_access.h"

namespace qol::game {

bool save_slot_meta_deps_ready() {
    return fn_save_get_save_slot != nullptr && fn_saveslot_get_hero != nullptr &&
           fn_save_destroy_save_slot != nullptr && fn_save_create_save_slot != nullptr;
}

void save_slots_refresh_if_stale() {
    ::save_slots_refresh_if_stale();
}

void save_slots_mark_stale() {
    ::save_slots_mark_stale();
}

void* save_slot_struct(int32_t slot) {
    return fn_save_get_save_slot != nullptr ? fn_save_get_save_slot(slot) : nullptr;
}

bool save_slot_exists(const void* slot_struct) {
    if (slot_struct == nullptr) return false;
    return reinterpret_cast<const uint8_t*>(slot_struct)[SAVESLOT_EXISTS] != 0;
}

int save_slot_hero_index(const void* slot_struct) {
    if (slot_struct == nullptr) return -1;
    return static_cast<int8_t>(
        reinterpret_cast<const uint8_t*>(slot_struct)[SAVESLOT_HERO_INDEX]);
}

void* save_slot_hero(void* slot_struct) {
    return fn_saveslot_get_hero != nullptr ? fn_saveslot_get_hero(slot_struct) : nullptr;
}

int save_hero_level(const void* hero) {
    if (hero == nullptr) return -1;
    return static_cast<int8_t>(reinterpret_cast<const uint8_t*>(hero)[C_LEVEL]);
}

int save_hero_class(const void* hero) {
    if (hero == nullptr) return -1;
    return static_cast<int8_t>(reinterpret_cast<const uint8_t*>(hero)[C_CLASS]);
}

bool save_load_deps_ready() {
    return fn_save_load_data != nullptr && fn_mem_free != nullptr;
}

int save_load_data(int32_t slot, void** out_buf, int* out_len) {
    if (fn_save_load_data == nullptr) return 0;
    return fn_save_load_data(slot, out_buf, out_len);
}

void save_mem_free(void* buf) {
    if (fn_mem_free != nullptr && buf != nullptr) fn_mem_free(buf);
}

const char* save_hub_get_key() {
    return fn_hub_save_get_key != nullptr ? fn_hub_save_get_key() : nullptr;
}

bool save_encrypt_available() {
    return fn_encrypt_process2 != nullptr;
}

int save_encrypt_process2(void* buf, int len, int mode, const char* key) {
    if (fn_encrypt_process2 == nullptr) return 0;
    return fn_encrypt_process2(buf, len, mode, key);
}

bool save_slot_delete_hook_target(void** out_slot, uintptr_t* out_expected) {
    if (g_base == 0 || out_slot == nullptr || out_expected == nullptr) return false;
    const uintptr_t offset = fn_resolve("F_SAVESLOT_DELETE_VMA", F_SAVESLOT_DELETE_VMA);
    if (offset == 0) return false;  // 符号未解析：fn_resolve 返回 0
    *out_slot = reinterpret_cast<void*>(g_base + G_SAVESLOT_DELETE_GOT_VMA);
    *out_expected = g_base + offset;
    return true;
}

}  // namespace qol::game
