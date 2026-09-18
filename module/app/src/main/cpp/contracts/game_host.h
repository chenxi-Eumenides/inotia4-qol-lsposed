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

struct QolGameHost {
    QolAbiHeader abi;
    QolTargetApi target;
    QolThreadApi thread;
    QolReadApi read;
    QolWriteApi write;
    QolCallApi call;
    QolUiPrimitiveApi ui_primitive;
};

constexpr uint16_t kGameHostMajor = 1;
constexpr uint16_t kGameHostMinor = 0;

}  // namespace qol::contracts
