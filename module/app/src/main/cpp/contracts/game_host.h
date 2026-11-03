#pragma once

// Game Host API ABI 骨架（ARCHITECTURE.md §2.1–§2.4、§3.8、§6.1；ADR-0008）。
//
// `game/` 是 host 内部的 adapter/实现，插件不得 include。只读 read 面可按调用点标注
// 线程使用；默认 write 与有副作用 call 只能由 owner 插件的主线程命令处理器调用，表内不
// 提供同步等待原语。入口 hook wrapper 在游戏主线程同步执行时，为完成被 hook 函数的语义
// 修改，可直接调用对应 write 面；范围仅限该 wrapper 语义所需字段，不得跨线程或扩大为通用写入口。
// 内置表可在 P1–P4 迁移期演进，P6 才冻结对外兼容。

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "host_sdk.h"

namespace qol::contracts {

constexpr uint64_t kQolTargetCapabilityC18NativeBagLayoutCandidate = 1ull << 3;  // 配对签名候选，仍需 2×5 拓扑准入。

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
                              QolItemHandle item, int32_t* out_value);
    QolResult (*jewel_value)(int32_t attribute_type, QolItemHandle item,
                             int32_t* out_value);
    QolResult (*uses_fixed_option_table)(int32_t category, int32_t* out_value);
    QolResult (*fixed_option_percent_pair)(int32_t category, int32_t option_index,
                                           int32_t* out_p1, int32_t* out_p2);
};
// C18L v507+ 的 MakeJewel 用固定表掷宝石数值，不再经 CAL 公式随机；
// 该尾表返回游戏实际使用的闭区间 [min,max]（category 28..32、词条 0..16；
// 17..35 走补词条表）。非 C18L、非宝石类别或越界词条返回 Unsupported；只读，不消耗游戏 RNG。
struct QolAttrRangeC18Api {
    QolResult (*jewel_generation_range)(int32_t attribute_type, QolItemHandle item,
                                        int32_t* out_min, int32_t* out_max);
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
    // Game Adapter 持有编码字段布局；插件只读写不透明字段值，不接触偏移。
    QolResult (*read_encoded_field)(const void* item, uint32_t* out_field);
    QolResult (*write_encoded_field)(void* item, uint32_t field);
    // 999 扩展模式策略（Host-owned，尾追加）：OK0=请求明确关闭；OK1=请求运行且整组资源
    // 已实际应用；NotReady=请求开启但安装/适配未满足，不伪装关闭。模式写权仅 Host 内部。
    QolResult (*read_extended_mode)(uint8_t* out_extended);
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
static_assert(offsetof(QolItemCountApi, read_extended_mode) ==
                  sizeof(QolItemCountApi::canonical) * 11,
              "QolItemCountApi ABI layout changed");
static_assert(sizeof(QolItemCountApi) == sizeof(QolItemCountApi::canonical) * 12,
              "QolItemCountApi ABI size changed");

// 宿主只暴露按操作语义命名的写原语；实现负责目标校验、线程纪律和失败返回。
struct QolCharacterWriteApi {
    QolResult (*max_hp)(void* character, int32_t value);
    QolResult (*hp)(void* character, int32_t value);
    QolResult (*set_equipped_item)(void* character, int32_t slot, void* item);
    // 原版 CHAR_ResetEquipItem：清空装备槽并重算角色状态。
    QolResult (*reset_equipped_item)(void* character, int32_t slot);
};

struct QolInventoryWriteApi {
    QolResult (*remove_item_data)(int32_t category, int32_t count);
    QolResult (*remove_item)(QolItemHandle item);
    QolResult (*create_perfect_item)(int32_t category, void** out_item);
    QolResult (*create_item)(int32_t category, int32_t arg1, int32_t arg2, int32_t arg3,
                             void** out_item);
    QolResult (*save_item)(void* item, void* param, int32_t* out_saved);
    QolResult (*free_item)(void* item);
    QolResult (*remove_item_direct)(int32_t bag, int32_t slot);
    // INVEN_FindSaveSlot 找到目标后写入的全局袋索引（0x736A00）。
    // 扩展袋使用扁平袋号；写入前必须先经 set_saved_bag。
    QolResult (*saved_bag)(int32_t* out_bag);
    QolResult (*set_saved_bag)(int32_t bag);
};

// Game-owned hook bridge. The handler is installed at the game ABI entry and returns the native
// pointer only to the original game caller; plugin code receives and exchanges item identities as handles.
using QolFindItemAbiFn = void* (*)(int32_t category);
struct QolItemAbiDispatchApi {
    QolFindItemAbiFn find_item_wrapper;
    QolResult (*bind_find_item_backup)(QolFindItemAbiFn backup);
    QolResult (*original_find_item_slot)(QolItemHandle item, int32_t* out_bag,
                                         int32_t* out_slot);
    QolResult (*item_identity_at)(int32_t bag, int32_t slot, QolItemHandle* out_item);
    void (*begin_original_only)();
    void (*end_original_only)();
    // Hook-private ABI passthroughs; returned pointers are valid only within the current hook call.
    QolResult (*native_item_at)(int32_t bag, int32_t slot, void** out_item);
    QolResult (*native_item_for_handle)(QolItemHandle item, void** out_item);
    // 快捷栏物品条目类别；仅 type=2 的物品快捷栏返回成功。
    QolResult (*shortcut_item_category)(int32_t shortcut, int32_t* out_category);
};

// 游戏身份只读面（v2.4 尾追加）：设置页展示用；字符串为静态存储或进程内有效。
struct QolGameIdentity {
    const char* series_name;    // "unknown"/"original"/"overhaul"/"monster"/"c18"
    const char* version_label;  // 变体表静态标签，可能为空串
    const char* game_version_name;  // 宿主游戏 APK versionName；未下发为 nullptr
    const char* android_release;    // 宿主 Android 版本（Build.VERSION.RELEASE）；未下发为 nullptr
    int32_t android_sdk_int;        // 宿主 Android API level；未下发为 0
    const char* package_name;   // 启动期下发的目标包名；未下发为 nullptr
    uint8_t known;              // 1 = 命中内置变体表
};
using QolGameIdentityFn = QolResult (*)(QolGameIdentity* out);
struct QolGameIdentityApi {
    QolGameIdentityFn read;
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
    // 关闭物品详情面板（原版出售按钮创建确认框前调用；只隐藏面板，不清详情上下文）。
    QolResult (*desc_set_off)();
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
    // ITEMPACKBASE：category 作为盒子时解包产物所需槽位总数。
    // out_slots < 0 表示该 category 不是拆包表中的盒子或表未就绪；>= 0 为所需槽位。
    QolResult (*pack_required_slots)(int32_t category, int32_t* out_slots);
    // INVEN_FindSaveSlot 的写入域：0 = 原版袋 0..4，1 = 任务袋 5 + C18 追加袋 6..17。
    // out_domain < 0 表示表未就绪；调用方应保持原版行为。
    QolResult (*native_save_domain)(int32_t category, int32_t* out_domain);
    // 装备槽表 record+2 bit4：1 = 双手武器。
    QolResult (*is_two_handed)(int32_t category, int32_t* out_value);
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
    QolItemAbiDispatchApi item_abi;
    QolGameIdentityApi game_identity;
    QolAttrRangeC18Api attr_range_c18;
};

static_assert(std::is_standard_layout<QolGameHost>::value,
              "QolGameHost must remain standard layout");
static_assert(std::is_trivially_copyable<QolGameHost>::value,
              "QolGameHost must remain trivially copyable");
static_assert(offsetof(QolGameHost, read_only) ==
                  offsetof(QolGameHost, item_count) + sizeof(QolItemCountApi),
              "QolGameHost read_only ABI offset changed");
static_assert(offsetof(QolGameHost, game_identity) ==
                  offsetof(QolGameHost, item_abi) + sizeof(QolItemAbiDispatchApi),
              "QolGameHost game_identity ABI offset changed");
static_assert(offsetof(QolGameHost, attr_range_c18) ==
                  offsetof(QolGameHost, game_identity) + sizeof(QolGameIdentityApi),
              "QolGameHost attr_range_c18 ABI offset changed");
static_assert(sizeof(QolGameHost) ==
                  offsetof(QolGameHost, attr_range_c18) + sizeof(QolAttrRangeC18Api),
              "QolGameHost ABI size changed");

constexpr uint16_t kGameHostMajor = 2;
// 2.4 appended the read-only game identity tail used by the settings page.
constexpr uint16_t kGameHostIdentityMinor = 4;
constexpr uint16_t kGameHostShortcutMinor = 5;
constexpr uint16_t kGameHostPackSlotsMinor = 6;
constexpr uint16_t kGameHostSavedBagMinor = 7;
constexpr uint16_t kGameHostTwoHandedMinor = 8;
// 2.9 appended the C18L gem generation-range tail used by attribute_quality.
constexpr uint16_t kGameHostC18JewelRangeMinor = 9;
constexpr uint16_t kGameHostMinor = kGameHostC18JewelRangeMinor;

}  // namespace qol::contracts
