#pragma once

// 扩展背包能力契约 v3.x（ADR-0007 §1，ARCHITECTURE.md §3.4 / §4）。
//
// 契约限制：POD + 固定宽度 C 类型 + 函数指针；禁止 STL、异常、虚函数、std::string。
// 字符串不跨契约边界（状态 JSON 的 `write_json(char*, uint32_t)` 为 v1.0 预留注释，
// 本版不导出）。
//
// 约定：
//   - bool 返回值语义 = 谓词命中 / 动作成功性；仅 `sell_price` 用负数错误码；
//   - 出参由调用方提供；无字符串参数；
//   - 全部成员仅游戏主线程调用；
//   - `for_each_logical_item` 的回调在持 `g_virtual_bag_mtx` 内执行，回调内禁止调用
//     本表的加锁成员（避免重入）；
//   - 能力撤销后禁止继续调用，禁止跨帧缓存 iface 或其中的裸指针。

#include <cstdint>

#include "result.h"
#include "item_handle.h"
#include "inventory_extension.h"

namespace qol::contracts {

constexpr const char* kExtensionBagCapabilityId = "extension_bag";
constexpr int32_t kExtensionBagVersionMajor = 3;
// Minimum version for the existing prefix; tail users must negotiate the exact 3.1 feature.
constexpr int32_t kExtensionBagVersionMinor = 0;
constexpr uint16_t kExtensionBagSaleRemoveVersionMinor = 1;
constexpr uint16_t kExtensionBagPublishedVersionMinor = 1;

// 返回 1 表示移动操作成功；返回 0 表示输出参数无效或移动操作失败。
// 输出指针有效时，body 为不附加 NUL 的 JSON；仅复制 min(capacity, body.size()) 字节，
// out_len 始终报告完整 body 长度，因此 body 可能被截断。成功 body 仅为可忽略的诊断信息，
// 调用方不得把它当作完整 JSON 使用；缓冲区不足不得将已执行的操作改报失败。此固定宽度
// 写面避免 std::string 跨契约边界。
using ExtensionBagMoveFn = int32_t (*)(int32_t from_bag, int32_t from_slot,
                                       int32_t to_bag, int32_t to_slot, char* out,
                                       uint32_t capacity, uint32_t* out_len);
using ExtensionBagCapacityFn = bool (*)(int32_t logical_bag, int32_t* out_capacity);
using ExtensionBagRemoveForSaleFn = QolResult (*)(int32_t logical_bag, int32_t slot,
                                                  QolItemHandle expected_item);

using ExtensionBagItemFn = QolInventoryExtensionItemFn;
using ExtensionBagPutJewelFn = QolResult (*)(int32_t role, int32_t logical_bag, int32_t slot,
                                             int32_t equip_slot);

enum class ExtensionBagDropResult : uint8_t {
    kNotExtension = 0,
    kHandled = 1,
    kRejected = 2,
};

enum class ExtensionBagEquipButtonResult : uint8_t {
    kNotExtension = 0,
    kHandled = 1,
    kBlocked = 2,
};

enum class ExtensionBagEquipEventResult : uint8_t {
    kNotExtension = 0,
    kHandled = 1,
    kBlocked = 2,
};

using ExtensionBagEquipEventBackup = uint64_t (*)(void*, uint64_t, void*, void*);
using ExtensionBagPutJewelBackup = int32_t (*)(QolItemHandle, QolItemHandle);

struct ExtensionBagMoveObservation {
    bool extension_item;
    int32_t extension_bag;
    int32_t extension_slot;
    uint32_t extension_handle;
    int32_t ownership_state;
    int32_t module_view_index;
    bool drag_session_active;
    int32_t source_bag;
    int32_t source_slot;
    QolItemHandle source_item;
    QolItemHandle target_item;
    uint64_t physical_digest;
    int32_t physical_nonnull;
    bool physical_valid;
};

struct ExtensionBagApi {
    // 1 状态/索引
    bool (*enabled)();
    bool (*module_view_installed)();
    bool (*is_inventory_enter)(uintptr_t enter);
    bool (*is_logical_bag)(int32_t bag);
    int32_t (*internal_index)(int32_t logical_bag);
    bool (*prepare_main_menu)();
    bool (*prepare_save_slot_load)();
    int32_t (*inventory_state_id)();
    void (*for_each_logical_item)(ExtensionBagItemFn fn, void* ctx);

    // 2 对象反查
    int32_t (*item_at)(int32_t internal_bag, int32_t slot, QolItemHandle* out_item);
    int32_t (*view_item_at)(int32_t bag, int32_t slot, QolItemHandle* out_item);
    bool (*identify_native_item)(QolItemHandle item, int32_t* out_bag, int32_t* out_slot);
    int32_t (*find_native_item)(int32_t category, QolItemHandle* out_item);
    bool (*remove_native_item)(QolItemHandle item);

    // 3 投影
    void (*sync_projected_slot)(int32_t bag, int32_t slot);
    void (*sync_projected_bag)();
    void (*store_restore_for_original_write)();

    // 4 收编/消耗
    bool (*consume_native_item)(QolItemHandle item);
    int32_t (*consume_category)(int32_t category, int32_t count);
    bool (*has_empty_slots)(int32_t needed, int32_t include_task_bag);
    bool (*adopt_unequipped_item)(void* character, int32_t equip_slot);
    bool (*adopt_native_item)(QolItemHandle item);
    bool (*merge_native_item)(QolItemHandle item);
    int64_t (*sell_price)(QolItemHandle item, int32_t count, bool apply_variant_discount);
    bool (*equip_projected_item)(void* character, QolItemHandle item, int32_t source_bag,
                                 int32_t source_slot, int32_t equip_slot);

    // 5 交互接管
    ExtensionBagEquipButtonResult (*handle_backpack_button_equip_result)();
    bool (*handle_confirm_use_item)(QolItemHandle item);
    bool (*handle_use_mercenary_seal)();
    ExtensionBagEquipEventResult (*handle_equip_control_event)(
        void* control, uint64_t event, void* x2, void* param,
        ExtensionBagEquipEventBackup backup, uint64_t* out_result);
    bool (*handle_original_bag_unequip)(bool* out_no_space, bool* out_not_empty);
    int32_t (*put_jewel_native)(QolItemHandle equip_item, QolItemHandle jewel_item,
                                ExtensionBagPutJewelBackup backup);

    // 6 观测/拖放（仅跨插件消费者）
    bool (*capture_move_item_observation)(QolItemHandle item, int32_t target_bag,
                                          int32_t target_slot,
                                          ExtensionBagMoveObservation* out);

    // 7 内部标记
    void (*begin_internal_equip)();
    void (*end_internal_equip)();
    bool (*internal_equip_active)();
    bool (*native_call_active)();
    void (*refresh_item_area_with_gate)();

    // v2.0 移动写面；调用方必须每次从 capability registry 获取接口，不得缓存。
    ExtensionBagMoveFn move_item;

    // v2.1 typed inventory snapshot support; capacity is the active logical-bag slot limit.
    ExtensionBagCapacityFn logical_bag_capacity;

    // v2.2 complete logical-bag jewel transaction; token ownership and consumption stay private.
    ExtensionBagPutJewelFn put_jewel;

    // v1.0 预留：状态 JSON 导出
    // int32_t (*write_json)(char* out, uint32_t capacity);

    // v3.1：整堆出售删除；仅在本次 capability require 协商到 >=3.1 后可读取。
    ExtensionBagRemoveForSaleFn remove_for_sale;
};

}  // namespace qol::contracts
