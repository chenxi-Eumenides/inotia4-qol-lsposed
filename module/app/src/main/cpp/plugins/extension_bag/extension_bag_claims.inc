// ===========================================================================
// D2a-3b-1：扩展背包 8 条声明式资源（claims + acquire/release + backup 交还）
// ===========================================================================
//
// 由 game_ui_virtbag.cpp 末尾 include（此时 render/lifecycle/store/mix/equip 的
// wrapper 与全局均已定义）。声明表经 extension_bag_bridge.h 交 plugins/extension_bag。
//
// 本 inc 只做「声明式接线」：wrapper 业务体逐字未动；PopupState 占用使用 D1-a2
// 部分 wrap 语义（空槽保留原值），event 槽按 hooks/popup_state 契约做 32 位返回适配。
// C1–C21 call patch 不在本批（a3b-2 收口），故 inject 线程保留。

#include <cstddef>
#include <cstring>

#include "contracts/plugin.h"

// ---- 资源 id（诊断/桥接按 id 分发）----
constexpr const char* kEbM1SaveInventory = "hook:SAVE_SaveInventory";
constexpr const char* kEbM2StoreSellItem = "hook:UIStore_SellItem";
constexpr const char* kEbP1InventoryPopup = "own:popup.inventory";
constexpr const char* kEbP2SavePopup = "own:popup.save_panel";
constexpr const char* kEbP3StorePopup = "own:popup.store";
constexpr const char* kEbP4MixPopup = "own:popup.mix";
constexpr const char* kEbS1DescSlots = "own:desc_button_execute_slots";
constexpr const char* kEbX1MoveMerge = "ptrslot:MOVE_MERGE_GOT";
constexpr const char* kEbGateKey = "extensionBagEnabled";

// PopupState 占用条目：资源事务 acquire 写入、release 写回（主线程）。
::qol::contracts::QolPopupStateEntry g_inventory_popup_entry{};
::qol::contracts::QolPopupStateEntry g_save_popup_entry{};
::qol::contracts::QolPopupStateEntry g_store_popup_entry{};
::qol::contracts::QolPopupStateEntry g_mix_popup_entry{};

// 从安装前快照读取函数指针/整数（部分 wrap 与原生 backup 同源）。
template <typename T>
T extension_bag_backup_value(const uint8_t* backup, size_t offset) {
    T value = nullptr;
    std::memcpy(&value, backup + offset, sizeof(value));
    return value;
}
// event 回调适配：游戏事件回调返回 uintptr_t，hooks/popup_state 契约固定 32 位返回值。
uint32_t virtual_bag_event_32(uint64_t event, uint64_t param, uint64_t param2) {
    return static_cast<uint32_t>(virtual_bag_event(event, param, param2));
}
uint32_t store_event_32(uint64_t event, uint64_t param, uint64_t param2) {
    return static_cast<uint32_t>(store_event_wrapper(event, param, param2));
}

// ---- P1：背包 PopupState（enter/f3/event 部分 wrap；process/f4 保留原值）----
int32_t extension_bag_inventory_popup_acquire(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_inventory_popup_entry.slot != nullptr) return 0;
    ::qol::contracts::QolPopupStateEntry entry{};
    if (g_sdk == nullptr || g_sdk->hooks.popup_resolve == nullptr ||
        g_sdk->hooks.popup_resolve("F_PANEL_INVENTORY_ENTER", &entry).ok != kQolOk) return 1;
    const ::qol::contracts::QolPopupStateHooks h{
        &virtual_bag_inventory_enter_wrapper,
        nullptr,
        &virtual_bag_f3_wrapper,
        nullptr,
        &virtual_bag_event_32,
    };
    if (g_sdk->hooks.popup_acquire == nullptr ||
        g_sdk->hooks.popup_acquire(&entry, &h).ok != kQolOk) return 1;
    g_inventory_popup_entry = entry;
    g_state_entry = entry.slot;
    g_orig_enter = extension_bag_backup_value<PopupNoArgFn>(
        entry.backup, kPopupStateEntryEnterOffset);
    g_inventory_enter = reinterpret_cast<uintptr_t>(g_orig_enter);
    g_orig_f3 = extension_bag_backup_value<PopupNoArgFn>(entry.backup, kPopupStateEntryF3Offset);
    g_orig_event = extension_bag_backup_value<PopupEventFn>(
        entry.backup, kPopupStateEntryEventOffset);
    return 0;
}
void extension_bag_inventory_popup_release(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_inventory_popup_entry.slot != nullptr && g_sdk != nullptr &&
        g_sdk->hooks.popup_restore != nullptr) {
        (void)g_sdk->hooks.popup_restore(&g_inventory_popup_entry);
    }
    g_state_entry = nullptr;
    g_orig_enter = nullptr;
    g_inventory_enter = 0;
    g_orig_f3 = nullptr;
    g_orig_event = nullptr;
}

// ---- P2：存档槽 PopupState（仅 enter 部分 wrap）----
int32_t extension_bag_save_popup_acquire(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_save_popup_entry.slot != nullptr) return 0;
    ::qol::contracts::QolPopupStateEntry entry{};
    if (g_sdk == nullptr || g_sdk->hooks.popup_resolve == nullptr ||
        g_sdk->hooks.popup_resolve("F_PANEL_SAVE_SLOT_ENTER", &entry).ok != kQolOk) return 1;
    const ::qol::contracts::QolPopupStateHooks h{
        &virtual_bag_save_panel_enter_wrapper, nullptr, nullptr, nullptr, nullptr};
    if (g_sdk->hooks.popup_acquire == nullptr ||
        g_sdk->hooks.popup_acquire(&entry, &h).ok != kQolOk) return 1;
    g_save_popup_entry = entry;
    g_save_state_entry = entry.slot;
    g_orig_save_enter = extension_bag_backup_value<PopupNoArgFn>(
        entry.backup, kPopupStateEntryEnterOffset);
    g_save_panel_hook_installed = true;
    return 0;
}
void extension_bag_save_popup_release(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_save_popup_entry.slot != nullptr && g_sdk != nullptr &&
        g_sdk->hooks.popup_restore != nullptr) {
        (void)g_sdk->hooks.popup_restore(&g_save_popup_entry);
    }
    g_save_state_entry = nullptr;
    g_orig_save_enter = nullptr;
    g_save_panel_hook_installed = false;
}

// ---- P3：商店 PopupState（enter/f3/event 部分 wrap）----
int32_t extension_bag_store_popup_acquire(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_store_popup_entry.slot != nullptr) return 0;
    ::qol::contracts::QolPopupStateEntry entry{};
    if (g_sdk == nullptr || g_sdk->hooks.popup_resolve == nullptr ||
        g_sdk->hooks.popup_resolve("F_PANEL_SHOP_ENTER", &entry).ok != kQolOk) return 1;
    const ::qol::contracts::QolPopupStateHooks h{
        &store_enter_wrapper,
        nullptr,
        &store_f3_wrapper,
        nullptr,
        &store_event_32,
    };
    if (g_sdk->hooks.popup_acquire == nullptr ||
        g_sdk->hooks.popup_acquire(&entry, &h).ok != kQolOk) return 1;
    g_store_popup_entry = entry;
    store_state_entry = entry.slot;
    store_original_enter =
        extension_bag_backup_value<StoreSceneNoArgFn>(entry.backup, kPopupStateEntryEnterOffset);
    store_original_f3 =
        extension_bag_backup_value<StoreSceneNoArgFn>(entry.backup, kPopupStateEntryF3Offset);
    store_original_event =
        extension_bag_backup_value<StoreSceneEventFn>(entry.backup, kPopupStateEntryEventOffset);
    return 0;
}
void extension_bag_store_popup_release(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_store_popup_entry.slot != nullptr && g_sdk != nullptr &&
        g_sdk->hooks.popup_restore != nullptr) {
        (void)g_sdk->hooks.popup_restore(&g_store_popup_entry);
    }
    store_state_entry = nullptr;
    store_original_enter = nullptr;
    store_original_f3 = nullptr;
    store_original_event = nullptr;
}

// ---- P4：合成 PopupState（enter/f3 部分 wrap）----
int32_t extension_bag_mix_popup_acquire(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_mix_popup_entry.slot != nullptr) return 0;
    ::qol::contracts::QolPopupStateEntry entry{};
    if (g_sdk == nullptr || g_sdk->hooks.popup_resolve == nullptr ||
        g_sdk->hooks.popup_resolve("F_PANEL_CRAFT_ENTER", &entry).ok != kQolOk) return 1;
    const ::qol::contracts::QolPopupStateHooks h{
        &mix_enter_wrapper, nullptr, &mix_f3_wrapper, nullptr, nullptr};
    if (g_sdk->hooks.popup_acquire == nullptr ||
        g_sdk->hooks.popup_acquire(&entry, &h).ok != kQolOk) return 1;
    g_mix_popup_entry = entry;
    mix_state_entry = entry.slot;
    mix_original_enter = extension_bag_backup_value<MixSceneNoArgFn>(
        entry.backup, kPopupStateEntryEnterOffset);
    mix_original_f3 = extension_bag_backup_value<MixSceneNoArgFn>(
        entry.backup, kPopupStateEntryF3Offset);
    return 0;
}
void extension_bag_mix_popup_release(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (g_mix_popup_entry.slot != nullptr && g_sdk != nullptr &&
        g_sdk->hooks.popup_restore != nullptr) {
        (void)g_sdk->hooks.popup_restore(&g_mix_popup_entry);
    }
    mix_state_entry = nullptr;
    mix_original_enter = nullptr;
    mix_original_f3 = nullptr;
}

// ---- S1：desc 动态实例槽命名空间占位（per-item 安装仍是业务 raw 写入）----
int32_t extension_bag_desc_slots_acquire(const char* id) {
    (void)id;
    // 仅登记命名空间所有权；per-item 绑定由 Host SDK 动态按钮接口管理。
    return 0;
}
void extension_bag_desc_slots_release(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    reset_extension_desc_item_hooks_locked();
}

const ::qol::contracts::OwnershipOps kEbInventoryPopupOps{&extension_bag_inventory_popup_acquire,
                                                        &extension_bag_inventory_popup_release};
const ::qol::contracts::OwnershipOps kEbSavePopupOps{&extension_bag_save_popup_acquire,
                                                   &extension_bag_save_popup_release};
const ::qol::contracts::OwnershipOps kEbStorePopupOps{&extension_bag_store_popup_acquire,
                                                    &extension_bag_store_popup_release};
const ::qol::contracts::OwnershipOps kEbMixPopupOps{&extension_bag_mix_popup_acquire,
                                                  &extension_bag_mix_popup_release};
const ::qol::contracts::OwnershipOps kEbDescSlotsOps{&extension_bag_desc_slots_acquire,
                                                   &extension_bag_desc_slots_release};

namespace qol {
namespace features {
namespace extension_bag {

const ::qol::contracts::ResourceClaim* extension_bag_claims(uint32_t* out_count) {
    // M1/M2 校正（D2a-3b-1）：ADR-0007 表记录 required × failClosed；按晚就绪语义
    // 校正为 required × failSoft —— 装配期失败保持 free + degraded，gate 开且 free
    // 时宿主每逻辑帧重试到成功，语义等价旧 300ms 重试线程。
    static const ::qol::contracts::ResourceClaim kClaims[] = {
        {
            kEbM1SaveInventory,
            "extension.save_inventory",
            ::qol::contracts::ResourceMechanism::kEntryHook,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft,
            true,
            "extension_bag",
            "SAVE_SaveInventory 是存档直调入口，投影安装期间必须还原投影再落盘（调用方无关）",
            kEbGateKey,
            120,
            reinterpret_cast<void*>(&save_inventory_wrapper),
        },
        {
            kEbM2StoreSellItem,
            "extension.store_sell_item",
            ::qol::contracts::ResourceMechanism::kEntryHook,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft,
            true,
            "extension_bag",
            "商店出售入口按弹窗载荷反推数量后接管整堆/部分出售；原版物品经 backup 透传",
            kEbGateKey,
            120,
            reinterpret_cast<void*>(&store_sell_item_wrapper),
        },
        {
            kEbP1InventoryPopup,
            kEbP1InventoryPopup,
            ::qol::contracts::ResourceMechanism::kOwnership,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft,
            true,
            "extension_bag",
            "背包 PopupState 条目需换模块 enter/f3/event（部分 wrap），投影/拖拽接管依赖此三回调",
            kEbGateKey,
            120,
            reinterpret_cast<void*>(const_cast<::qol::contracts::OwnershipOps*>(&kEbInventoryPopupOps)),
        },
        {
            kEbP2SavePopup,
            kEbP2SavePopup,
            ::qol::contracts::ResourceMechanism::kOwnership,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft,
            true,
            "extension_bag",
            "存档槽 PopupState 条目 enter 需换模块 wrapper（世界内直接存档路径）",
            kEbGateKey,
            120,
            reinterpret_cast<void*>(const_cast<::qol::contracts::OwnershipOps*>(&kEbSavePopupOps)),
        },
        {
            kEbP3StorePopup,
            kEbP3StorePopup,
            ::qol::contracts::ResourceMechanism::kOwnership,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft,
            true,
            "extension_bag",
            "商店 PopupState 条目需换模块 enter/f3/event（部分 wrap），商店投影与袋列遮蔽依赖此三回调",
            kEbGateKey,
            120,
            reinterpret_cast<void*>(const_cast<::qol::contracts::OwnershipOps*>(&kEbStorePopupOps)),
        },
        {
            kEbP4MixPopup,
            kEbP4MixPopup,
            ::qol::contracts::ResourceMechanism::kOwnership,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft,
            true,
            "extension_bag",
            "合成 PopupState 条目需换模块 enter/f3（部分 wrap），合成投影与袋列遮蔽依赖此两回调",
            kEbGateKey,
            120,
            reinterpret_cast<void*>(const_cast<::qol::contracts::OwnershipOps*>(&kEbMixPopupOps)),
        },
        {
            kEbS1DescSlots,
            kEbS1DescSlots,
            ::qol::contracts::ResourceMechanism::kOwnership,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft,
            true,
            "extension_bag",
            "11 个动态 desc 实例槽命名空间占位；per-item 安装由 Host SDK 动态绑定器管理",
            kEbGateKey,
            120,
            reinterpret_cast<void*>(const_cast<::qol::contracts::OwnershipOps*>(&kEbDescSlotsOps)),
        },
        {
            kEbX1MoveMerge,
            kEbX1MoveMerge,
            ::qol::contracts::ResourceMechanism::kPtrSlot,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft,
            true,
            "extension_bag",
            "UIEquip 物品事件 proc GOT 槽常驻 wrapper（move_merge / 扩展源保护在 wrapper 内自门控）",
            nullptr,
            120,
            reinterpret_cast<void*>(&ui_equip_inven_item_proc_wrapper),
        },
        // ---- D2a-3b-2：C1–C8 存档 callsite ----
        // 站点已在 a3a 登记（game/access/patches 的 P_EXT_SAVE_*）；handler 为模块存档编排
        // 机制 owner=extension_bag；行为经 Host SDK 存档面执行。
        {
            "patch:P_EXT_SAVE_REVIVE", "P_EXT_SAVE_REVIVE",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "存档 callsite 重定向到模块存档编排；投影安装期间必须还原投影再落盘",
            kEbGateKey, 120, reinterpret_cast<void*>(&extension_bag_save_game_handler),
        },
        {
            "patch:P_EXT_SAVE_QUEST_CLEAR_1", "P_EXT_SAVE_QUEST_CLEAR_1",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "存档 callsite 重定向到模块存档编排；投影安装期间必须还原投影再落盘",
            kEbGateKey, 120, reinterpret_cast<void*>(&extension_bag_save_game_handler),
        },
        {
            "patch:P_EXT_SAVE_QUEST_CLEAR_2", "P_EXT_SAVE_QUEST_CLEAR_2",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "存档 callsite 重定向到模块存档编排；投影安装期间必须还原投影再落盘",
            kEbGateKey, 120, reinterpret_cast<void*>(&extension_bag_save_game_handler),
        },
        {
            "patch:P_EXT_SAVE_QUEST_REVIEW", "P_EXT_SAVE_QUEST_REVIEW",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "存档 callsite 重定向到模块存档编排；投影安装期间必须还原投影再落盘",
            kEbGateKey, 120, reinterpret_cast<void*>(&extension_bag_save_game_handler),
        },
        {
            "patch:P_EXT_SAVE_PROCESS", "P_EXT_SAVE_PROCESS",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "存档 callsite 重定向到模块存档编排；投影安装期间必须还原投影再落盘",
            kEbGateKey, 120, reinterpret_cast<void*>(&extension_bag_save_game_handler),
        },
        {
            "patch:P_EXT_SAVE_NETWORK_ADD", "P_EXT_SAVE_NETWORK_ADD",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "存档 callsite 重定向到模块存档编排；投影安装期间必须还原投影再落盘",
            kEbGateKey, 120, reinterpret_cast<void*>(&extension_bag_save_game_handler),
        },
        {
            "patch:P_EXT_SAVE_NETWORK_PROCESS_1", "P_EXT_SAVE_NETWORK_PROCESS_1",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "存档 callsite 重定向到模块存档编排；投影安装期间必须还原投影再落盘",
            kEbGateKey, 120, reinterpret_cast<void*>(&extension_bag_save_game_handler),
        },
        {
            "patch:P_EXT_SAVE_NETWORK_PROCESS_2", "P_EXT_SAVE_NETWORK_PROCESS_2",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "存档 callsite 重定向到模块存档编排；投影安装期间必须还原投影再落盘",
            kEbGateKey, 120, reinterpret_cast<void*>(&extension_bag_save_game_handler),
        },
        // ---- C9–C14 装配/背包绘制门禁（功能型 required；站点已在 a3a 登记）----
        {
            "patch:P_EXT_INVEN_ITEM_DRAW", "P_EXT_INVEN_ITEM_DRAW",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "扩展视图物品绘制 wrapper（投影物品/门禁复用原版渲染）",
            kEbGateKey, 120, reinterpret_cast<void*>(&virtual_bag_draw_inven_item_wrapper),
        },
        {
            "patch:P_EXT_INVEN_BAG_DRAW", "P_EXT_INVEN_BAG_DRAW",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "原版袋绘制 wrapper：扩展视图下遮蔽原版袋高亮/内容",
            kEbGateKey, 120, reinterpret_cast<void*>(&virtual_bag_draw_original_bag_wrapper),
        },
        {
            "patch:P_EXT_INVEN_DRAW_END", "P_EXT_INVEN_DRAW_END",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "装备页绘制收尾 wrapper（页签/投影收尾与主菜单清理）",
            kEbGateKey, 120, reinterpret_cast<void*>(&virtual_bag_draw_end_wrapper),
        },
        {
            "patch:P_EXT_DROP_GATE", "P_EXT_DROP_GATE",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "落空槽 drop 门禁 wrapper（投影期间路由扩展事务，抑制原版写入）",
            kEbGateKey, 120, reinterpret_cast<void*>(&save_item_on_empty_gate),
        },
        {
            "patch:P_EXT_DRAW_GATE", "P_EXT_DRAW_GATE",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "ITEM_DrawPorting 绘制门禁 wrapper（悬空指针/未知物品拦截，防崩）",
            kEbGateKey, 120, reinterpret_cast<void*>(&item_draw_porting_gate),
        },
        {
            "patch:P_EXT_DESC_OPEN_GATE", "P_EXT_DESC_OPEN_GATE",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "物品 desc 打开入口 wrapper（desc 菜单装备按钮 hook 安装点）",
            kEbGateKey, 120, reinterpret_cast<void*>(&make_desc_equip_gate),
        },
        // ---- C15–C19 商店宿主（装饰型 optional：缺省不终态化插件）----
        {
            "patch:P_EXT_STORE_DESC", "P_EXT_STORE_DESC",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kOptional,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "商店 desc 打开门禁 wrapper（扩展物品详情接管）",
            kEbGateKey, 120, reinterpret_cast<void*>(&store_make_desc_gate),
        },
        {
            "patch:P_EXT_STORE_DRAW_END", "P_EXT_STORE_DRAW_END",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "商店绘制收尾 wrapper（页签/投影收尾）",
            kEbGateKey, 120, reinterpret_cast<void*>(&store_draw_end_wrapper),
        },
        {
            "patch:P_EXT_STORE_BAG_DRAW", "P_EXT_STORE_BAG_DRAW",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kOptional,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "商店袋列 B 尾调用 wrapper（R-57 原版袋列高亮遮蔽；站点 kind=kBranchRedirect）",
            kEbGateKey, 120, reinterpret_cast<void*>(&store_draw_inven_bag_group_wrapper),
        },
        {
            "patch:P_EXT_STORE_BUY_GATE_1", "P_EXT_STORE_BUY_GATE_1",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "买入预检门禁 wrapper：原版袋无空位但扩展袋有空位时放行收编",
            kEbGateKey, 120, reinterpret_cast<void*>(&store_buy_find_slot_gate),
        },
        {
            "patch:P_EXT_STORE_BUY_GATE_2", "P_EXT_STORE_BUY_GATE_2",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "买入预检门禁 wrapper：原版袋无空位但扩展袋有空位时放行收编",
            kEbGateKey, 120, reinterpret_cast<void*>(&store_buy_find_slot_gate),
        },
        // ---- C20–C21 合成宿主（C21 装饰型 optional）----
        {
            "patch:P_EXT_MIX_DRAW_END", "P_EXT_MIX_DRAW_END",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kRequired,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "合成绘制收尾 wrapper（页签/投影收尾）",
            kEbGateKey, 120, reinterpret_cast<void*>(&mix_draw_end_wrapper),
        },
        {
            "patch:P_EXT_MIX_BAG_DRAW", "P_EXT_MIX_BAG_DRAW",
            ::qol::contracts::ResourceMechanism::kCallPatch,
            ::qol::contracts::ResourcePolicy::kPrivate,
            ::qol::contracts::ResourceRequirement::kOptional,
            ::qol::contracts::ResourceFailure::kFailSoft, true, "extension_bag",
            "合成袋列 wrapper（R-57 原版袋列高亮遮蔽）",
            kEbGateKey, 120, reinterpret_cast<void*>(&mix_draw_inven_bag_group_wrapper),
        },
    };
    if (out_count != nullptr) {
        *out_count = static_cast<uint32_t>(sizeof(kClaims) / sizeof(kClaims[0]));
    }
    return kClaims;
}

int32_t extension_bag_bind_backup(const char* resource_id, void* backup) {
    if (resource_id == nullptr) return 1;
    if (std::strcmp(resource_id, kEbM1SaveInventory) == 0) {
        if (backup == nullptr) return 1;
        g_backup_save_inventory = backup;
        return 0;
    }
    if (std::strcmp(resource_id, kEbM2StoreSellItem) == 0) {
        if (backup == nullptr) return 1;
        g_backup_store_sell_item = backup;
        return 0;
    }
    // X1 指针槽 wrapper 直走 game_access 解析入口，接收即成功（无需缓存 backup）。
    if (std::strcmp(resource_id, kEbX1MoveMerge) == 0) return 0;
    return 1;
}

bool extension_bag_set_enabled(bool enabled) {
    return set_virtual_bag_enabled(enabled);
}
bool extension_bag_enabled() {
    return virtual_bag_enabled();
}

}  // namespace extension_bag
}  // namespace features
}  // namespace qol
