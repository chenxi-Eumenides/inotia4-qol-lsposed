#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"
#include "../item_handle.h"

namespace qol::contracts {

// 1.3：尾部追加原生词条写入口 write_native_option_value。
// 1.4：退役产物生命周期入口 create_perfect_item/create_item/save_item/free_item；
// 产物生成/入库/回滚统一走 inventory.action 的 create/place/discard_handle。
// 1.5：退役 write_jewel_word/set_max_socket_enchant/write_native_option_value；
// 物品字段改写统一走 core.item 的 item.payload（payload 纯数据）与
// inventory.transfer.replace_item（写槽），本端口只保留合成界面/材料写面与只读字段。
struct QolCustomRecipeWritePort {
    QolVersion abi_version;
    QolResult (*write_stuff_need)(uint32_t index, int32_t category, int32_t count);
    QolResult (*set_stuff_item)(uint32_t index, QolItemHandle item);
    QolResult (*description_set_off)();
    QolResult (*read_jewel_word)(QolItemHandle item, uint32_t* out_word);
    QolResult (*random_inclusive)(int32_t lo, int32_t hi, int32_t* out);
};

static_assert(std::is_standard_layout<QolCustomRecipeWritePort>::value,
              "QolCustomRecipeWritePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolCustomRecipeWritePort>::value,
              "QolCustomRecipeWritePort must remain trivially copyable");

}  // namespace qol::contracts
