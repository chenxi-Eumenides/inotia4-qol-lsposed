#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

// 物品/背包写面的私有适配端口（ADR 0001/阶段 5 写面收口）：仅 core.item 可注入。
// 参数统一用固定宽度 POD（QolItemWriteArgs），避免变参与裸指针；数值语义见各 kind 注释。
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

// 写函数：主线程执行；成功写 out（迁移前原响应 JSON）并置 out_size，失败返回非 ok
// （out 可携带原错误体）。返回值语义与其它 adapter port 一致。
using QolItemWriteFn = QolResult (*)(const QolItemWriteArgs* args, char* out, uint32_t capacity,
                                    uint32_t* out_size);

struct QolItemWritePortReadApi {
    QolItemWriteFn write;
};

struct QolItemWritePort {
    QolVersion abi_version;
    QolItemWritePortReadApi read;
};

static_assert(std::is_standard_layout<QolItemWritePort>::value,
              "QolItemWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemWritePort>::value,
              "QolItemWritePort must remain trivially copyable");
static_assert(std::is_standard_layout<QolItemWriteArgs>::value,
              "QolItemWriteArgs must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemWriteArgs>::value,
              "QolItemWriteArgs must remain trivially copyable");

}  // namespace qol::contracts
