#pragma once

#include <cstdint>
#include <type_traits>

namespace qol::contracts {

// 物品/背包写面的命令 payload（固定宽度 POD）。原 QolItemWritePort 低层端口已随 T2c-3
// 删除：所有 item 写 kind 均由 core.item owner 结果处理器经 typed 低层 op 执行。
// 参数语义见各 kind 注释。
enum class QolItemWriteKind : int32_t {
    kUseItem = 0,      // i0=bag, i1=slot
    kSellItem = 1,     // i0=bag, i1=slot
    kMoveItem = 2,     // i0=bag, i1=slot, i2=count, i3=to_bag, i4=to_slot
    kDiscardItem = 3,  // i0=bag, i1=slot
    kAcceptDice = 4,   // 无参
    kRejectDice = 5,   // 无参
    kEquipItem = 6,    // i0=role, i1=bag, i2=slot
    kUnequipItem = 7,  // i0=role, i1=slot
    kPutJewel = 8,     // i0=role, i1=bag, i2=slot, i3=equip_slot
    kEnchant = 9,      // i0=role, i1=bag, i2=slot, i3=equip_slot
    kShopBuy = 10,     // i0=shop_slot
    kAddItem = 11,     // i0=category, i1=count, i2=socket, i3=enhance_remaining, i4=rarity
    kSetMoney = 12,    // i64=money
};

struct QolItemWriteArgs {
    int32_t kind = 0;
    int32_t i0 = 0;
    int32_t i1 = 0;
    int32_t i2 = 0;
    int32_t i3 = 0;
    int32_t i4 = 0;
    int64_t i64 = 0;
    int32_t i5 = -1;  // kEquipItem category（负值表示按 bag/slot 装备）
};

static_assert(std::is_standard_layout<QolItemWriteArgs>::value,
              "QolItemWriteArgs must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemWriteArgs>::value,
              "QolItemWriteArgs must remain trivially copyable");

}  // namespace qol::contracts
