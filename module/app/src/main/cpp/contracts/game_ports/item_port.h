#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>

#include "../result.h"
#include "../version.h"
#include "../item_handle.h"
#include "../item_count.h"
#include "../sell_price.h"

namespace qol::contracts {

struct QolInventoryExtensionAccess;

enum class QolInventoryItemKind : int32_t {
    kOriginal = 0,
    kProvider = 1,
};

struct QolInventoryItemRef {
    int32_t kind;
    int32_t bag;
    int32_t slot;
    int32_t category;
    int32_t count;
    QolItemHandle item;
};

// Atomic snapshot of the selected inventory detail slot. The control object stays in Game Adapter.
struct QolItemDetailContext {
    int32_t desc_type;
    int32_t bag;
    int32_t slot;
    QolItemHandle item;
};

using QolInventoryBagSlotFn = int32_t (*)(const QolInventoryItemRef* item, void* ctx);
using QolItemJsonFn = QolResult (*)(char* out, uint32_t capacity, uint32_t* out_size);
using QolItemPortQuoteFn = QolResult (*)(QolItemHandle item, uint32_t sale_path,
                                         QolSaleQuote* out);

constexpr uint32_t kQolItemInventoryOptionCapacity = 32;
constexpr uint32_t kQolItemInventoryNativeOptionCapacity = 32;
constexpr uint32_t kQolItemInventoryJewelOptionCapacity = 32;
// 附魔属性列表尚无解码来源（当前只读 I_ENCHANT 的 id/level）；容量先按合并列表同等上限
// 固定，数据未解码前适配器保持 count=0 且不置有效位，不得用 id/level 伪造属性。
constexpr uint32_t kQolItemInventoryEnchantOptionCapacity = 32;
// Autosell view consumer table (plugins/autosell/view.cpp): real-equip→equip; rarity→rarity;
// remaining-enchant→enchant_remaining; filled/total sockets→socket_filled/socket_total;
// jewel value/type→jewel_value/jewel_attribute_type; quantity uses effective_count when
// count_supported is set. Bag category remains QolInventoryItemRef.category. Consumers do not
// reinterpret payload or recompute the R-47b mode view.

// 装备 option 节点 O_INDEX bit13-15 的条目类型。合并列表 options[] 保留全部类型；分词列表
// 只投影这里已定义的类型，未知 type 仍只出现在合并列表。
enum class QolItemInventoryOptionType : int32_t {
    kNative = 0,
    kJewel = 1,
};

enum QolItemInventoryViewField : uint32_t {
    kQolItemInventoryHasCategory = 1u << 0,
    kQolItemInventoryHasRarity = 1u << 1,
    kQolItemInventoryHasDamage = 1u << 2,
    kQolItemInventoryHasDefense = 1u << 3,
    kQolItemInventoryHasNeedLevel = 1u << 4,
    kQolItemInventoryHasJewelValue = 1u << 5,
    kQolItemInventoryHasJewelAttributeType = 1u << 6,
    kQolItemInventoryHasOptionCompleteness = 1u << 7,
    kQolItemInventoryOptionsTruncated = 1u << 8,
    // 分词列表可用位：原生/宝石列表由本次枚举投影得到（可为空）；附魔无解码数据时不置位。
    kQolItemInventoryHasNativeOptions = 1u << 9,
    kQolItemInventoryHasJewelOptions = 1u << 10,
    kQolItemInventoryHasEnchantOptions = 1u << 11,
};

struct QolItemInventoryOptionView {
    int32_t value;
    int32_t id;
    int32_t type;
};

struct QolItemInventoryView {
    uint32_t fields;
    int32_t type_flags;
    int32_t raw_rarity;
    int32_t category;
    int32_t count_supported;
    int32_t effective_count;
    int32_t equip;
    int32_t rarity;
    int32_t damage;
    int32_t defense;
    int32_t magic_rate;
    int32_t need_level;
    int32_t socket;
    int32_t socket_filled;
    int32_t socket_total;
    int32_t enchant;
    int32_t enchant_remaining;
    int32_t chaos;
    int32_t enchant_id;
    int32_t enchant_level;
    int32_t chaos_level;
    int32_t chaos_rate;
    int32_t jewel_value;
    int32_t jewel_attribute_type;
    uint32_t option_count;
    QolItemInventoryOptionView options[kQolItemInventoryOptionCapacity];
    // v3.5 追加的分词列表。options[] 仍是全量合并列表，既有消费者保持原序语义；
    // 原生/宝石列表按 type 投影；附魔列表当前显式为空（count=0 且无有效位），
    // 元素暂沿用通用词条结构，待属性数据解码后再细化。
    uint32_t native_option_count;
    QolItemInventoryOptionView native_options[kQolItemInventoryNativeOptionCapacity];
    uint32_t jewel_option_count;
    QolItemInventoryOptionView jewel_options[kQolItemInventoryJewelOptionCapacity];
    uint32_t enchant_option_count;
    QolItemInventoryOptionView enchant_options[kQolItemInventoryEnchantOptionCapacity];
};

static_assert(std::is_standard_layout<QolItemInventoryView>::value,
              "QolItemInventoryView must remain standard layout");
static_assert(std::is_trivially_copyable<QolItemInventoryView>::value,
              "QolItemInventoryView must remain trivially copyable");

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
    QolResult (*category)(QolItemHandle item, int32_t* out_category);
    QolResult (*is_real_equip)(QolItemHandle item, int32_t* out_value);
    QolResult (*is_jewel_category)(int32_t category, int32_t* out_is_jewel);
    QolResult (*is_no_sell)(int32_t category, int32_t* out_value);
    QolResult (*sell_price)(QolItemHandle item, int64_t* out_price);
    QolResult (*cumulate_count)(QolItemHandle item, int32_t* out_count);
    QolResult (*item_ref_at)(int32_t bag, int32_t slot, QolInventoryItemRef* out);
    QolResult (*physical_item_at)(int32_t bag, int32_t slot, QolItemHandle* out_item);
    QolResult (*detail_item_context)(QolItemDetailContext* out_context);
    // Legacy JSON adapter retained for unmigrated game-system write/cache consumers.
    QolItemJsonFn inventory_json;
    QolResult (*money)(int64_t* out_money);
    QolItemJsonFn shop_items_json;
    QolResult (*in_world)(int32_t* out_in_world);
    QolItemStaticTextFn static_text;
    // 静态名称表是否已由启动期下发（owner 快照据此推迟发布，避免输出空名称）。
    QolResult (*static_ready)(int32_t* out_ready);
    // Synchronous main-thread typed view; item pointers remain inside the adapter call path.
    QolResult (*inventory_item_view)(QolItemHandle item, QolItemInventoryView* out_view);
    QolResult (*effective_cumulate_count)(QolItemHandle item, int32_t* out_count);
    QolResult (*inventory_extension_access)(QolInventoryExtensionAccess* out_access);
    QolResult (*count_limit)(QolItemHandle item, uint32_t* out_limit);
    QolResult (*count_encoding)(QolItemHandle item, QolItemCountEncoding* out_encoding);
    QolItemPortQuoteFn quote;
};

struct QolItemPort {
    QolVersion abi_version;
    uint32_t struct_size;
    QolItemPortReadApi read;
};

inline bool qol_item_port_size_valid(const QolItemPort* port) {
    constexpr uint32_t kLegacySize = static_cast<uint32_t>(
        offsetof(QolItemPort, read) + offsetof(QolItemPortReadApi, count_encoding));
    return port != nullptr && port->struct_size >= kLegacySize;
}

inline bool qol_item_port_count_encoding_size_valid(const QolItemPort* port) {
    constexpr uint32_t kRequiredSize = static_cast<uint32_t>(
        offsetof(QolItemPort, read) + offsetof(QolItemPortReadApi, count_encoding) +
        sizeof(QolItemPortReadApi::count_encoding));
    return port != nullptr && port->struct_size >= kRequiredSize;
}

inline bool qol_item_port_quote_size_valid(const QolItemPort* port) {
    constexpr uint32_t kRequiredSize = static_cast<uint32_t>(
        offsetof(QolItemPort, read) + offsetof(QolItemPortReadApi, quote) +
        sizeof(QolItemPortQuoteFn));
    return port != nullptr && port->struct_size >= kRequiredSize;
}

}  // namespace qol::contracts
