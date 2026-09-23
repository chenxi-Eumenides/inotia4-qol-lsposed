#pragma once

#include <cstdint>

#include "../result.h"
#include "../version.h"

namespace qol::contracts {

enum class QolInventoryItemKind : int32_t {
    kOriginal = 0,
    kExtension = 1,
};

struct QolInventoryItemRef {
    int32_t kind;
    int32_t bag;
    int32_t slot;
    int32_t category;
    int32_t count;
    void* native_item;
};

using QolInventoryBagSlotFn = int32_t (*)(const QolInventoryItemRef* item, void* ctx);
using QolItemJsonFn = QolResult (*)(char* out, uint32_t capacity, uint32_t* out_size);

// 启动期由 Kotlin 下发的静态名称表类别（见 StaticData → JNI → adapter port）。
enum class QolItemStaticTextKind : int32_t {
    kItemName = 0,
    kItemType = 1,
    kOptionName = 2,
    kEnchantEffect = 3,
    // 技能名（action_id → 名称）：与物品名同为启动期静态文本；character_facts 的
    // party/{slot}/skills 富化需要它，复用同一行式下发通道以避免为单条查询新增 JNI。
    kSkillName = 4,
};
// 返回的是 JSON 已转义、不含外层引号的文本指针；存储只在启动期写入，运行期只读。
using QolItemStaticTextFn = QolResult (*)(int32_t kind, int32_t id, const char** out_text);

struct QolItemPortReadApi {
    QolResult (*category)(const void* item, int32_t* out_category);
    QolResult (*is_real_equip)(const void* item, int32_t* out_value);
    QolResult (*is_jewel_category)(int32_t category, int32_t* out_is_jewel);
    QolResult (*is_no_sell)(int32_t category, int32_t* out_value);
    QolResult (*sell_price)(const void* item, int64_t* out_price);
    QolResult (*cumulate_count)(const void* item, int32_t* out_count);
    QolResult (*item_ref_at)(int32_t bag, int32_t slot, QolInventoryItemRef* out);
    QolResult (*physical_item_at)(int32_t bag, int32_t slot, void** out_item);
    QolResult (*detail_context)(int32_t* out_desc_type, int32_t* out_bag, void** out_control);
    QolResult (*cursor_index)(const void* control, int32_t* out_slot);
    // 只在游戏主线程调用，由 item_facts 发布为快照；HTTP handler 不直接调用这些函数。
    QolItemJsonFn inventory_json;
    QolResult (*money)(int64_t* out_money);
    QolItemJsonFn shop_items_json;
    QolResult (*in_world)(int32_t* out_in_world);
    QolItemStaticTextFn static_text;
    // 静态名称表是否已由启动期下发（owner 快照据此推迟发布，避免输出空名称）。
    QolResult (*static_ready)(int32_t* out_ready);
};

struct QolItemPort {
    QolVersion abi_version;
    QolItemPortReadApi read;
};

}  // namespace qol::contracts
