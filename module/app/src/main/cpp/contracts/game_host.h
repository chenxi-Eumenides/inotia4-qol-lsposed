#pragma once

// Game Host API ABI 骨架（ARCHITECTURE.md §2.1–§2.4、§3.8、§6.1；ADR-0008）。
//
// `game/` 是 host 内部的 adapter/实现，插件不得 include。只读 read 面可按调用点标注
// 线程使用；write 与有副作用 call 只能由 owner 插件的主线程命令处理器调用，表内不
// 提供同步等待原语。内置表可在 P1–P4 迁移期演进，P6 才冻结对外兼容。

#include <cstdint>

#include "contracts/host_sdk.h"

namespace qol::contracts {

struct QolTargetVariant {
    uint32_t id;
    uint32_t capabilities;
};
struct QolTargetApi {
    QolResult (*query_variant)(QolTargetVariant* out);
    int32_t (*has_capability)(uint64_t bit);
    int32_t (*symbol_available)(const char* symbol_id);
};

struct QolThreadApi {
    int32_t (*is_main_thread)();
    int64_t (*frame_count)();
};

struct QolCharReadApi {
    QolResult (*type)(const void* character, int32_t* out_type);
    QolResult (*is_active_player_group)(const void* character, int32_t* out_active);
    QolResult (*party_index)(const void* character, int32_t* out_index);
};
struct QolItemReadApi {
    QolResult (*category)(const void* item, int32_t* out_category);
    QolResult (*is_jewel_category)(int32_t category, int32_t* out_is_jewel);
    QolResult (*ability_level)(const void* item, int32_t* out_level);
};
struct QolWorldReadApi {
    QolResult (*position)(const void* world, int32_t* out_x, int32_t* out_y);
    QolResult (*state)(int32_t* out_state);
};
struct QolCraftReadApi {
    QolResult (*ui_type)(uint32_t* out_type);
    QolResult (*panel_state)(int32_t* out_state);
    QolResult (*mix_type)(uint32_t* out_type);
    QolResult (*selected_stuff_index)(int64_t* out_index);
    QolResult (*selected_stuff_item)(void** out_item);
    QolResult (*stuff_item)(uint32_t index, void** out_item);
    QolResult (*category_held_count)(int32_t category, int32_t* out_count);
    QolResult (*character_level)(int32_t* out_level);
};
struct QolSaveReadApi {
    QolResult (*slot_state)(int32_t slot, int32_t* out_state);
};
struct QolQuestReadApi {
    QolResult (*state)(int32_t* out_state);
};
struct QolAttrRangeReadApi {
    QolResult (*option_value)(int32_t option_index, int32_t level, int32_t flag,
                              const void* item, int32_t* out_value);
    QolResult (*jewel_value)(int32_t attribute_type, const void* item, int32_t* out_value);
    QolResult (*uses_fixed_option_table)(int32_t category, int32_t* out_value);
    QolResult (*fixed_option_percent_pair)(int32_t category, int32_t option_index,
                                           int32_t* out_p1, int32_t* out_p2);
};
struct QolReadApi {
    QolCharReadApi character;
    QolItemReadApi item;
    QolWorldReadApi world;
    QolCraftReadApi craft;
    QolSaveReadApi save;
    QolQuestReadApi quest;
    QolAttrRangeReadApi attr_range;
};

struct QolCraftWriteApi {
    QolResult (*write_stuff_need)(uint32_t index, int32_t category, int32_t count);
    QolResult (*set_stuff_item)(uint32_t index, void* item);
    QolResult (*set_target_slot_item)(void* item);
    QolResult (*set_cost_zero)();
};
struct QolWriteApi {
    QolCraftWriteApi craft;
};

struct QolCallApi {
    QolResult (*call_known)(uint32_t call_id, const void* args, uint32_t args_size,
                            void* out, uint32_t out_size);
};

struct QolUiPrimitiveApi {
    QolResult (*create_control)(uint32_t kind, void** out_control);
    QolResult (*set_rect)(void* control, int32_t left, int32_t top, int32_t right, int32_t bottom);
    int32_t (*hit_test)(const void* control, int32_t x, int32_t y);
    QolResult (*draw_text)(void* canvas, const char* text, uint32_t text_len);
    QolResult (*draw_rect)(void* canvas, int32_t left, int32_t top, int32_t right, int32_t bottom);
    QolResult (*draw_image)(void* canvas, const void* image, int32_t x, int32_t y);
    QolResult (*insert_inline_color)(void* builder, uint32_t max_text_bytes, uint8_t color_code);
    QolResult (*popup_show)(uint32_t popup_id);
    QolResult (*popup_close)(uint32_t popup_id);
};

// L4-0 追加面：只覆盖已审计插件迁移所需的低层游戏原语。该面仍是宿主内核 ABI，
// 不暴露 game/ 内部类型、符号常量或 STL；有副作用的成员只能由 owner 插件的主线程
// 命令处理器调用。
struct QolCharExtraReadApi {
    QolResult (*is_party_member)(const void* character, int32_t* out_member);
    QolResult (*summoner)(const void* character, void** out_summoner);
    QolResult (*level)(const void* character, int32_t* out_level);
    QolResult (*merc_slot)(const void* character, int32_t* out_slot);
    QolResult (*can_change_equip)(const void* character, int32_t* out_allowed);
    QolResult (*menu_character)(void** out_character);
    QolResult (*pool_slot)(const void* character, int32_t* out_slot);
    QolResult (*max_hp)(const void* character, int32_t* out_value);
    QolResult (*hp)(const void* character, int32_t* out_value);
};

struct QolCharExtraWriteApi {
    QolResult (*max_hp)(void* character, int32_t value);
    QolResult (*hp)(void* character, int32_t value);
};

struct QolItemExtraReadApi {
    QolResult (*rarity)(const void* item, int32_t* out_rarity);
    QolResult (*is_real_equip)(const void* item, int32_t* out_value);
    QolResult (*equip_level)(const void* item, int32_t* out_level);
    QolResult (*is_no_sell)(int32_t category, int32_t* out_value);
    QolResult (*is_dice)(int32_t category, int32_t* out_value);
    QolResult (*cumulate_count)(const void* item, int32_t* out_count);
    QolResult (*class_no_equip_bit)(int32_t category, int32_t* out_value);
    QolResult (*equip_desc_role_type)(int32_t* out_type);
};

struct QolInventoryReadApi {
    QolResult (*bit_range)(int32_t value, int32_t high_bit, int32_t low_bit, int32_t* out_value);
};

struct QolInventoryWriteApi {
    QolResult (*remove_item_data)(int32_t category, int32_t count);
    QolResult (*remove_item)(void* item);
    QolResult (*create_perfect_item)(int32_t category, void** out_item);
    QolResult (*create_item)(int32_t category, int32_t arg1, int32_t arg2, int32_t arg3,
                             void** out_item);
    QolResult (*save_item)(void* item, void* param, int32_t* out_saved);
    QolResult (*free_item)(void* item);
};

struct QolCraftExtraReadApi {
    QolResult (*stuff_slot_control)(uint32_t index, void** out_control);
    QolResult (*target_slot_item)(void** out_item);
    QolResult (*selected_inventory_item)(void** out_item);
    QolResult (*selected_recipe)(int32_t type, int64_t* out_recipe);
    QolResult (*stuff_slot_count)(uint32_t* out_count);
};

struct QolCraftExtraCallApi {
    QolResult (*init_mixing_state)();
    QolResult (*reset_stuff_item_control)();
    QolResult (*refresh_inventory_items)();
    QolResult (*select_first_empty_stuff_slot)(int32_t* out_index);
    QolResult (*set_ui_type)(int64_t type);
    QolResult (*set_selected_recipe)(int32_t type, int64_t recipe);
    QolResult (*play_sound)(int32_t sound_id);
    QolResult (*show_text)(uint32_t text_id);
    QolResult (*show_yesno_from_text)(uint32_t text_id, void* ok, void* cancel);
    QolResult (*description_set_off)();
};

struct QolWorldExtraReadApi {
    QolResult (*map_record_count)(int32_t* out_count);
    QolResult (*current_map_id)(int32_t* out_map_id);
    QolResult (*map_name)(int32_t map_id, const char** out_name);
    QolResult (*active_player_direction)(int32_t* out_direction);
    QolResult (*money)(int64_t* out_money);
};

struct QolWorldExtraCallApi {
    QolResult (*create_yesno)(const char* text, uint32_t text_len, void* ok, void* cancel,
                              void* param);
    QolResult (*minus_money)(int64_t amount);
    QolResult (*request_map_change)(int32_t map_id, int32_t x, int32_t y, int32_t direction);
    QolResult (*close_popup)();
    QolResult (*restore_hud)();
    QolResult (*show_banner)(const char* text);
};

struct QolSaveExtraReadApi {
    QolResult (*slot_exists)(int32_t slot, int32_t* out_exists);
    QolResult (*slot_hero_index)(int32_t slot, int32_t* out_index);
    QolResult (*hero_level)(int32_t slot, int32_t* out_level);
    QolResult (*hero_class)(int32_t slot, int32_t* out_class);
    // 成功后 out_data 由宿主分配，调用方必须经 save_call.free_data 释放；out_key 由宿主持有。
    QolResult (*load_data)(int32_t slot, void** out_data, int32_t* out_size);
    QolResult (*hub_key)(const char** out_key);
};

struct QolSaveExtraCallApi {
    QolResult (*free_data)(void* data);
    QolResult (*encrypt)(void* data, int32_t size, int32_t mode, const char* key,
                         int32_t* out_result);
    QolResult (*mark_stale)();
};

struct QolQuestExtraReadApi {
    QolResult (*active_slot_count)(uint32_t* out_count);
    QolResult (*active_slot_id)(uint32_t index, uint32_t* out_id);
    QolResult (*total_count)(uint32_t* out_count);
    QolResult (*state)(uint32_t quest_id, int32_t* out_state);
    QolResult (*is_complete)(uint32_t quest_id, int32_t* out_complete);
};

struct QolQuestExtraWriteApi {
    QolResult (*set_state)(uint32_t quest_id, int32_t state);
};

struct QolChoiceExtraReadApi {
    QolResult (*state_id)(int32_t* out_state_id);
    QolResult (*selected_index)(int32_t* out_index);
    QolResult (*text_owns)(const char* const* items, uint32_t count, int32_t* out_owned);
};

struct QolChoiceExtraCallApi {
    QolResult (*write_items)(const char* const* items, uint32_t count);
    QolResult (*write_title)(const char* title);
    QolResult (*write_focus)(int32_t index);
    QolResult (*push)();
    QolResult (*close_popup)();
    QolResult (*play_sound)(int32_t sound_id);
};

struct QolGameHostExtraApi {
    QolCharExtraReadApi character;
    QolCharExtraWriteApi character_write;
    QolItemExtraReadApi item;
    QolInventoryReadApi inventory;
    QolInventoryWriteApi inventory_write;
    QolCraftExtraReadApi craft;
    QolCraftExtraCallApi craft_call;
    QolWorldExtraReadApi world;
    QolWorldExtraCallApi world_call;
    QolSaveExtraReadApi save;
    QolSaveExtraCallApi save_call;
    QolQuestExtraReadApi quest;
    QolQuestExtraWriteApi quest_write;
    QolChoiceExtraReadApi choice;
    QolChoiceExtraCallApi choice_call;
};

struct QolGameHost {
    QolAbiHeader abi;
    QolTargetApi target;
    QolThreadApi thread;
    QolReadApi read;
    QolWriteApi write;
    QolCallApi call;
    QolUiPrimitiveApi ui_primitive;
    QolGameHostExtraApi extra;
};

constexpr uint16_t kGameHostMajor = 1;
constexpr uint16_t kGameHostMinor = 1;

}  // namespace qol::contracts
