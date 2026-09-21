#include "game/access/world.h"

#include "game/access/game_access.h"

namespace qol::game {

bool world_ready() {
    return g_base != 0;
}

int world_state() {
    if (g_state == nullptr) return -1;
    return static_cast<int>(*reinterpret_cast<uint16_t*>(g_state));
}

int world_game_state_gate() {
    if (g_gamestate == nullptr) return 0;
    return static_cast<int>(*reinterpret_cast<uint32_t*>(g_gamestate));
}

int world_map_record_count() {
    if (g_mapinfo_record_count == nullptr ||
        !game_memory_accessible(g_mapinfo_record_count, sizeof(uint16_t), 'r')) {
        return 0;
    }
    return static_cast<int>(*reinterpret_cast<uint16_t*>(g_mapinfo_record_count));
}

const char* world_map_name(int map_id) {
    if (g_mapinfo_pdata_got == nullptr || g_mapinfo_record_size == nullptr ||
        g_mapinfo_record_count == nullptr || fn_memorytext_get_text == nullptr) {
        return nullptr;
    }
    auto* pdata_slot = reinterpret_cast<void**>(g_mapinfo_pdata_got);
    if (!game_memory_accessible(pdata_slot, sizeof(void*), 'r') || *pdata_slot == nullptr) {
        return nullptr;
    }
    auto* records = *reinterpret_cast<uint8_t**>(*pdata_slot);
    const uint8_t record_size = *reinterpret_cast<uint8_t*>(g_mapinfo_record_size);
    const uint16_t record_count = *reinterpret_cast<uint16_t*>(g_mapinfo_record_count);
    if (records == nullptr || record_size == 0 || map_id < 0 || map_id >= record_count) {
        return nullptr;
    }
    auto* record = records + static_cast<size_t>(map_id) * record_size;
    if (!game_memory_accessible(record, record_size, 'r')) return nullptr;
    const uint16_t text_id = *reinterpret_cast<uint16_t*>(record + MAPINFOBASE_RECORD_NAME_TEXT_ID);
    const char* text = fn_memorytext_get_text(text_id);
    if (text == nullptr || !game_memory_accessible(text, 1, 'r') || text[0] == '\0') {
        return nullptr;
    }
    return text;
}

int world_current_map_id() {
    return static_cast<int>(current_map_id());
}

bool world_map_change_ready() {
    return fn_mapchange_set != nullptr && fn_gamestate_set_state != nullptr;
}

void world_request_map_change(int map_id, int x, int y, int dir) {
    if (!world_map_change_ready()) return;
    fn_mapchange_set(map_id, x, y, dir);
    fn_gamestate_set_state(static_cast<int32_t>(GAMESTATE_MAP_CHANGE));
}

int world_active_player_direction() {
    void* player = nullptr;
    if (g_player_active_got != nullptr &&
        game_memory_accessible(g_player_active_got, sizeof(void*), 'r')) {
        void* player_global = *reinterpret_cast<void**>(g_player_active_got);
        if (player_global != nullptr &&
            game_memory_accessible(player_global, sizeof(void*), 'r')) {
            player = *reinterpret_cast<void**>(player_global);
        }
    }
    // 某些版本的 GOT 槽直接保存角色对象；已有 PLAYER_pActivePlayer 全局作为安全回退。
    if (player == nullptr && g_player_active != nullptr &&
        game_memory_accessible(g_player_active, sizeof(void*), 'r')) {
        player = *reinterpret_cast<void**>(g_player_active);
    }
    if (player == nullptr) return -1;
    auto* bytes = reinterpret_cast<uint8_t*>(player);
    if (!game_memory_accessible(bytes + C_DIRECTION, sizeof(uint8_t), 'r')) return -1;
    return *reinterpret_cast<uint8_t*>(bytes + C_DIRECTION);
}

bool world_money_ready() {
    return fn_get_money != nullptr && fn_minus_money != nullptr;
}

int64_t world_money() {
    return fn_get_money != nullptr ? fn_get_money() : 0;
}

bool world_minus_money(int64_t amount) {
    return fn_minus_money != nullptr && fn_minus_money(amount) != 0;
}

bool world_create_yesno_ready() {
    return fn_popup_create_yesno != nullptr;
}

bool world_create_yesno(const char* text, uint32_t len, void* ok, void* cancel, void* param) {
    if (fn_popup_create_yesno == nullptr || text == nullptr) return false;
    fn_popup_create_yesno(const_cast<char*>(text), len, 0, 2, ok, cancel, param);
    return true;
}

bool world_close_popup_ready() {
    return fn_ui_set_popup_process_info != nullptr;
}

bool world_close_popup_process() {
    if (fn_ui_set_popup_process_info == nullptr) return false;
    fn_ui_set_popup_process_info(3, 0);
    return true;
}

void world_restore_hud_gate() {
    if (g_base == 0) return;
    auto** hud_gate = reinterpret_cast<uint8_t**>(g_base + G_HUD_GATE_GOT_VMA);
    if (hud_gate != nullptr && *hud_gate != nullptr) **hud_gate = 1;
}

bool world_show_banner(const char* text) {
    if (fn_instantmsg_add == nullptr || text == nullptr) return false;
    fn_instantmsg_add(3, const_cast<char*>(text), 0, 0, 5, 0x15, 0, 0);
    return true;
}

int world_choice_state_id() {
    if (g_base == 0) return -1;
    auto* list_slot = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (!game_memory_accessible(list_slot, sizeof(void*), 'r') || *list_slot == nullptr) {
        return -1;
    }
    auto* list = reinterpret_cast<uint8_t*>(*list_slot);
    for (size_t index = 0; index < POPUP_STATE_COUNT; ++index) {
        auto* entry = list + index * POPUP_ENTRY_SIZE;
        const uintptr_t enter = *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER);
        if (enter == g_base + F_PANEL_CHOICE_ENTER) {
            return static_cast<int>(*reinterpret_cast<uint32_t*>(entry));
        }
    }
    return -1;
}

bool world_teleport_patch_info(WorldTeleportPatch* out) {
    if (g_base == 0 || out == nullptr) return false;
    out->patch_addr = g_base + F_UI_PLAY_CALL_MAP_NAME_VMA + F_UI_PLAY_CALL_MAP_NAME_PATCH_OFF;
    out->return_addr = g_base + F_UI_PLAY_CALL_MAP_NAME_VMA + F_UI_PLAY_CALL_MAP_NAME_EPILOGUE_OFF;
    out->expected_word = 0xd2800020u;  // mov x0, #1
    out->legacy_target = g_base + F_UI_PLAY_CALL_MAP_NAME_LEGACY_TARGET_VMA;
    return true;
}

}  // namespace qol::game
