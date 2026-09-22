#pragma once

// Game Host API ABI 骨架（ARCHITECTURE.md §2.1–§2.4、§3.8、§6.1；ADR-0008）。
//
// `game/` 是 host 内部的 adapter/实现，插件不得 include。只读 read 面可按调用点标注
// 线程使用；write 与有副作用 call 只能由 owner 插件的主线程命令处理器调用，表内不
// 提供同步等待原语。内置表可在 P1–P4 迁移期演进，P6 才冻结对外兼容。

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "host_sdk.h"

namespace qol::contracts {

struct QolTargetVariant {
    uint32_t id;
    uint32_t capabilities;
};
enum class QolTargetFeatureState : int32_t {
    kUnknown = 0,
    kUnsupported = 1,
    kAvailable = 2,
    kUnavailable = 3,
};
struct QolTargetApi {
    QolResult (*query_variant)(QolTargetVariant* out);
    int32_t (*has_capability)(uint64_t bit);
    int32_t (*symbol_available)(const char* symbol_id);
    QolResult (*warehouse_companion_file_state)(QolTargetFeatureState* out);
};

struct QolThreadApi {
    int32_t (*is_main_thread)();
    int64_t (*frame_count)();
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
    QolAttrRangeReadApi attr_range;
};
enum class QolItemCountEncoding : uint8_t {
    kUnknown = 0,
    kNotEncoded = 1,
    kEncoded = 2,
};

static_assert(sizeof(QolItemCountEncoding) == sizeof(uint8_t),
              "QolItemCountEncoding underlying type changed");

struct QolItemCountApi {
    QolResult (*canonical)(const void* item, uint32_t* out_count);
    QolResult (*effective)(const void* item, uint32_t* out_count);
    QolResult (*is_encoded)(const void* item, QolItemCountEncoding* out_encoding);
    QolResult (*write_effective)(void* item, uint32_t count);
    QolResult (*writeback_needed)(const void* item, uint32_t count, int32_t* out_needed);
    QolResult (*field_from_canonical)(uint32_t field, uint32_t canonical, int32_t extended,
                                      uint32_t* out_field);
    QolResult (*canonical_from_field)(uint32_t field, uint32_t* out_canonical);
    QolResult (*view_from_canonical)(uint32_t canonical, int32_t extended, uint32_t* out_view);
    QolResult (*view_limit)(int32_t extended, uint32_t* out_limit);
    // provider 持有编码字段布局；插件只读写不透明字段值，不接触偏移。
    QolResult (*read_encoded_field)(const void* item, uint32_t* out_field);
    QolResult (*write_encoded_field)(void* item, uint32_t field);
};

static_assert(std::is_standard_layout<QolItemCountApi>::value,
              "QolItemCountApi must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemCountApi>::value,
              "QolItemCountApi must remain trivially copyable");
static_assert(offsetof(QolItemCountApi, canonical) == 0,
              "QolItemCountApi ABI layout changed");
static_assert(offsetof(QolItemCountApi, effective) == sizeof(QolItemCountApi::canonical),
              "QolItemCountApi ABI layout changed");
static_assert(offsetof(QolItemCountApi, is_encoded) == sizeof(QolItemCountApi::canonical) * 2,
              "QolItemCountApi ABI layout changed");
static_assert(offsetof(QolItemCountApi, write_effective) == sizeof(QolItemCountApi::canonical) * 3,
              "QolItemCountApi ABI layout changed");
static_assert(offsetof(QolItemCountApi, writeback_needed) == sizeof(QolItemCountApi::canonical) * 4,
              "QolItemCountApi ABI layout changed");
static_assert(offsetof(QolItemCountApi, field_from_canonical) ==
                  sizeof(QolItemCountApi::canonical) * 5,
              "QolItemCountApi ABI layout changed");
static_assert(offsetof(QolItemCountApi, canonical_from_field) ==
                  sizeof(QolItemCountApi::canonical) * 6,
              "QolItemCountApi ABI layout changed");
static_assert(offsetof(QolItemCountApi, view_from_canonical) ==
                  sizeof(QolItemCountApi::canonical) * 7,
              "QolItemCountApi ABI layout changed");
static_assert(offsetof(QolItemCountApi, view_limit) == sizeof(QolItemCountApi::canonical) * 8,
              "QolItemCountApi ABI layout changed");
static_assert(offsetof(QolItemCountApi, read_encoded_field) ==
                  sizeof(QolItemCountApi::canonical) * 9,
              "QolItemCountApi ABI layout changed");
static_assert(offsetof(QolItemCountApi, write_encoded_field) ==
                  sizeof(QolItemCountApi::canonical) * 10,
              "QolItemCountApi ABI layout changed");
static_assert(sizeof(QolItemCountApi) == sizeof(QolItemCountApi::canonical) * 11,
              "QolItemCountApi ABI size changed");

// 宿主只暴露按操作语义命名的写原语；实现负责目标校验、线程纪律和失败返回。
struct QolCharacterWriteApi {
    QolResult (*max_hp)(void* character, int32_t value);
    QolResult (*hp)(void* character, int32_t value);
    QolResult (*set_equipped_item)(void* character, int32_t slot, void* item);
};

struct QolInventoryWriteApi {
    QolResult (*remove_item_data)(int32_t category, int32_t count);
    QolResult (*remove_item)(void* item);
    QolResult (*create_perfect_item)(int32_t category, void** out_item);
    QolResult (*create_item)(int32_t category, int32_t arg1, int32_t arg2, int32_t arg3,
                             void** out_item);
    QolResult (*save_item)(void* item, void* param, int32_t* out_saved);
    QolResult (*free_item)(void* item);
    QolResult (*remove_item_direct)(int32_t bag, int32_t slot);
};

struct QolWorldActionApi {
    QolResult (*create_yesno)(const char* text, uint32_t text_len, void* ok, void* cancel,
                              void* param);
    QolResult (*minus_money)(int64_t amount);
    QolResult (*request_map_change)(int32_t map_id, int32_t x, int32_t y, int32_t direction);
    QolResult (*close_popup)();
    QolResult (*restore_hud)();
    QolResult (*show_banner)(const char* text);
    QolResult (*add_money)(int64_t amount);
    QolResult (*cancel_tutorial)();
};

// 按域只读面只提供事实查询；副作用不通过该面表达。
struct QolItemReadOnlyApi {
    QolResult (*rarity)(const void* item, int32_t* out_rarity);
    QolResult (*equip_level)(const void* item, int32_t* out_level);
    QolResult (*is_dice)(int32_t category, int32_t* out_value);
    QolResult (*class_no_equip_bit)(int32_t category, int32_t* out_value);
    QolResult (*equip_desc_role_type)(int32_t* out_type);
    QolResult (*enchant_remaining)(const void* item, int32_t* out_value);
    QolResult (*socket_filled)(const void* item, int32_t* out_value);
    QolResult (*socket_total)(const void* item, int32_t* out_value);
    QolResult (*is_backpack_category)(int32_t category, int32_t* out_value);
    // 宝石 item+0x10 bits0-10；原落点：plugins/autosell/view.cpp:77。
    QolResult (*jewel_value)(const void* item, int32_t* out_value);
    // 宝石 item+0x10 bits18-23；原落点：plugins/autosell/view.cpp:79。
    QolResult (*jewel_attribute_type)(const void* item, int32_t* out_type);
    QolResult (*is_enchant_scroll)(int32_t category, int32_t* out_value);
    QolResult (*is_mercenary_seal)(int32_t category, int32_t* out_value);
    QolResult (*is_usable)(int32_t category, int32_t* out_value);
    QolResult (*count_encoding)(int32_t category, QolItemCountEncoding* out_encoding);
};

struct QolReadOnlyApi {
    QolItemReadOnlyApi item;
};

static_assert(std::is_standard_layout<QolReadOnlyApi>::value,
              "QolReadOnlyApi must remain standard layout");
static_assert(std::is_trivially_copyable<QolReadOnlyApi>::value,
              "QolReadOnlyApi must remain trivially copyable");

struct QolGameHost {
    QolAbiHeader abi;
    QolTargetApi target;
    QolThreadApi thread;
    QolReadApi read;
    QolItemCountApi item_count;
    QolReadOnlyApi read_only;
    QolCharacterWriteApi character_write;
    QolInventoryWriteApi inventory_write;
    QolWorldActionApi world_action;
};

static_assert(std::is_standard_layout<QolGameHost>::value,
              "QolGameHost must remain standard layout");
static_assert(std::is_trivially_copyable<QolGameHost>::value,
              "QolGameHost must remain trivially copyable");
static_assert(offsetof(QolGameHost, read_only) ==
                  offsetof(QolGameHost, item_count) + sizeof(QolItemCountApi),
              "QolGameHost read_only ABI offset changed");
static_assert(sizeof(QolGameHost) ==
                  offsetof(QolGameHost, world_action) + sizeof(QolWorldActionApi),
              "QolGameHost ABI size changed");

constexpr uint16_t kGameHostMajor = 2;
constexpr uint16_t kGameHostMinor = 0;

}  // namespace qol::contracts
