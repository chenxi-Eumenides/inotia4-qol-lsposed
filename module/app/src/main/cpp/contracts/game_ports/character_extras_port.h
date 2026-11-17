#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"
#include "../item_handle.h"

namespace qol::contracts {

struct QolCharacterExtrasReadApi {
    QolResult (*party_index)(const void* character, int32_t* out_index);
    QolResult (*is_party_member)(const void* character, int32_t* out_member);
    QolResult (*summoner)(const void* character, void** out_summoner);
    QolResult (*level)(const void* character, int32_t* out_level);
    QolResult (*merc_slot)(const void* character, int32_t* out_slot);
    QolResult (*can_change_equip)(const void* character, int32_t* out_allowed);
    QolResult (*menu_character)(void** out_character);
    QolResult (*pool_slot)(const void* character, int32_t* out_slot);
    QolResult (*max_hp)(const void* character, int32_t* out_value);
    QolResult (*hp)(const void* character, int32_t* out_value);
    QolResult (*can_equip)(const void* character, const void* item, int32_t* out_allowed);
    QolResult (*find_equip_slot)(const void* character, const void* item, int32_t* out_slot);
    QolResult (*equipped_item)(const void* character, int32_t slot, void** out_item);
    QolResult (*has_empty_mercenary_slot)(int32_t* out_available);
    QolResult (*can_apply_stuff)(const void* target, const void* material, int32_t* out_allowed);
    QolResult (*party_member)(int32_t role, void** out_character);
    QolResult (*can_apply_stuff_items)(QolItemHandle target, QolItemHandle material,
                                       int32_t* out_allowed);
    uint32_t struct_size;
};

inline bool qol_character_extras_read_api_size_valid(const QolCharacterExtrasReadApi* api) {
    return api != nullptr && api->struct_size >= sizeof(QolCharacterExtrasReadApi);
}

struct QolCooldownApi {
    uint32_t struct_size;
    QolResult (*active_player)(void** out_character);
    QolResult (*now_tick)(int64_t* out_tick);
    QolResult (*potion_tick)(int64_t* out_tick);
    QolResult (*set_potion_tick)(int64_t tick);
};

inline bool qol_cooldown_api_size_valid(const QolCooldownApi* api) {
    return api != nullptr && api->struct_size >= sizeof(QolCooldownApi);
}

// 规则表访问（1.4 追加）：CONSTBASE 行 0 的角色/佣兵等级上限。
struct QolCharacterRulesApi {
    uint32_t struct_size;
    // 读行 0；表未装载返回 not_ready。
    QolResult (*level_cap_get)(int32_t* out_cap);
    // 写行 0；只接受 1..255；仅游戏主线程调用。
    QolResult (*level_cap_set)(int32_t cap);
};

inline bool qol_character_rules_api_size_valid(const QolCharacterRulesApi* api) {
    return api != nullptr && api->struct_size >= sizeof(QolCharacterRulesApi);
}

struct QolCharacterExtrasPort {
    QolVersion abi_version;
    QolCharacterExtrasReadApi read;
    // 冷却 tick 访问（1.3 追加）。旧消费者只读 read，不受尾部追加影响。
    QolCooldownApi cooldown;
    // 规则表访问（1.4 追加）。尾追加保证旧字段偏移不变。
    QolCharacterRulesApi rules;
};

}  // namespace qol::contracts
