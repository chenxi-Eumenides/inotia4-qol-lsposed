#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"
#include "../item_handle.h"

namespace qol::contracts {

struct QolCustomRecipeWritePort {
    QolVersion abi_version;
    QolResult (*write_stuff_need)(uint32_t index, int32_t category, int32_t count);
    QolResult (*set_stuff_item)(uint32_t index, QolItemHandle item);
    QolResult (*description_set_off)();
    QolResult (*create_perfect_item)(int32_t category, QolItemHandle* out_item);
    QolResult (*create_item)(int32_t category, int32_t arg1, int32_t arg2, int32_t arg3,
                             QolItemHandle* out_item);
    QolResult (*save_item)(QolItemHandle item, int32_t* out_saved);
    QolResult (*free_item)(QolItemHandle item);
    QolResult (*read_jewel_word)(QolItemHandle item, uint32_t* out_word);
    QolResult (*write_jewel_word)(QolItemHandle item, uint32_t word);
    QolResult (*set_max_socket_enchant)(QolItemHandle item);
    QolResult (*random_inclusive)(int32_t lo, int32_t hi, int32_t* out);
    // 改写装备第 `native_index` 条**原生词条**（O_INDEX bit13-15 == 0）的存储值。
    // 只统计原生词条节点；宝石词条（type 1）与附魔字段不在本入口的可见范围。
    // native_index 越界、链表不可读或 value 超出 int16 返回 NotFound/NotReady/InvalidArgument。
    QolResult (*write_native_option_value)(QolItemHandle item, uint32_t native_index,
                                           int32_t value);
};

static_assert(std::is_standard_layout<QolCustomRecipeWritePort>::value,
              "QolCustomRecipeWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolCustomRecipeWritePort>::value,
              "QolCustomRecipeWritePort must remain trivially copyable");

}  // namespace qol::contracts
