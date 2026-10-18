#pragma once

#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/runtime/state.h"

#include <cstdint>

enum class ExtensionBagEquipResult {
    kOk,
    kReject,         // 前置不满足：非背包物品 / 扩展位已装备 / 不在原版库存
    kFailed,         // 移除或回滚失败（状态可能不一致，强日志）
    kPersistFailed,  // persist 失败且已完整回滚
};

enum class ExtensionBagUnequipResult {
    kOk,
    kNotEquipped,
    kNotEmpty,
    kNoSpace,
    kPersistFailed,
    kFailed,
};

struct NativeCallScope {
    const bool previous = g_in_native_call;
    const qol::contracts::QolGameHost* game = runtime_state().host.game;

    NativeCallScope() {
        g_in_native_call = true;
        if (game != nullptr && game->item_abi.begin_original_only != nullptr) {
            game->item_abi.begin_original_only();
        }
    }
    ~NativeCallScope() {
        if (game != nullptr && game->item_abi.end_original_only != nullptr) {
            game->item_abi.end_original_only();
        }
        g_in_native_call = previous;
    }
};

// 触摸事件使用 Scene_Draw 的绝对逻辑坐标；真机运行时宽度为 1408。
constexpr int64_t kCellX = 0x4c4;
constexpr int64_t kCellY = 0x86;
// Phase one uses an independent read-only overlay. These coordinates stay in
// the same absolute logical space as the original equipment scene.
// UIEquip_CreateInvenControl absolute origin: bag button origin minus the
// bag container offset plus the item-container offset.
constexpr int64_t kGridX = 0x45c - 0x185 + 0x22; // 761
constexpr int64_t kGridY = 0x91 - 0x0c + 0x1b;   // 160
constexpr int64_t kGridCell = 0x4a;
constexpr int64_t kGridStep = 0x53;
constexpr int64_t kOriginalGridX = kGridX;
constexpr int64_t kOriginalGridY = kGridY;
constexpr int kEquipSlotCount = 10;

#include "plugins/extension_bag/runtime/access.h"
#include "plugins/extension_bag/runtime/ownership.h"
#include "plugins/extension_bag/runtime/tabs.h"
#include "plugins/extension_bag/runtime/projection.h"
#include "plugins/extension_bag/runtime/lifecycle.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/transaction/transaction.h"
#include "plugins/extension_bag/ui/host_ui.h"
