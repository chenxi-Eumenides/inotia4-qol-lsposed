#pragma once

// 装备/卸袋结果与装备槽数：纯模型，不依赖 UI 或 runtime。
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
