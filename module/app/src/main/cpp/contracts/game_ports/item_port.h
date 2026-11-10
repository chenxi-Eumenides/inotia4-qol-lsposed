#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>

#include "../item_type.h"
#include "../result.h"
#include "../version.h"
#include "../item_handle.h"
#include "../item_count.h"
#include "../sell_price.h"

namespace qol::contracts {

enum class QolInventoryItemKind : int32_t {
    kOriginal = 0,
    kProvider = 1,
};

// 袋编号空间：原版 0、改版（C18 追加袋）1，其余为注册源分配值（>=2）。
// 各空间索引一律从 0 起；扁平袋号仅是按 原版→改版→注册源 顺序连接的派生便利键。
constexpr int32_t kQolBagSpaceOriginal = 0;
constexpr int32_t kQolBagSpaceVariant = 1;
constexpr int32_t kQolBagSpaceFirstRegistered = 2;

// 原版固定 6 袋（0..4 事务袋 + 5 任务袋）；改版为动态袋数，索引上界排他。
constexpr int32_t kQolOriginalBagCount = 6;
constexpr int32_t kQolTaskBagIndex = 5;
// C18 袋对象表到槽表之间的指针容量：(0x735100 - 0x735000) / sizeof(void*)。
constexpr int32_t kQolMaxNativeBagCount = 32;

struct QolBagRef {
    int32_t space_id;
    int32_t bag;
};

struct QolBagInfo {
    int32_t capacity;
    int32_t occupied_slots;
};

// 原生袋布局事实。bag_limit 含原版 6 袋（非 C18 恒为 6）；c18_layout 表示 C18
// 原生袋局探测成功。只读，不含地址。
struct QolNativeBagLayout {
    int32_t bag_limit;
    int32_t c18_layout;
};

struct QolInventoryItemRef {
    int32_t kind;
    int32_t bag;
    int32_t slot;
    int32_t category;
    int32_t count;
    QolItemHandle item;
    // 袋编号空间；与 bag 一起构成稳定身份。旧字段顺序保持不变。
    int32_t space_id;
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

// option 链表枚举总量上限；三个分词列表各自独立容量。
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

// 装备 option 节点 O_INDEX bit13-15 的条目类型。分词列表只投影这里已定义的类型；
// 未知 type 不进入任何列表，避免误归类。
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
    // 装备词条按来源拆分。v3.6 起删除合并 options[]/option_count；三个列表均只包含
    // 已识别来源的条目，未知 type 不写入任何列表。
    uint32_t native_option_count;
    QolItemInventoryOptionView native_options[kQolItemInventoryNativeOptionCapacity];
    uint32_t jewel_option_count;
    QolItemInventoryOptionView jewel_options[kQolItemInventoryJewelOptionCapacity];
    // 附魔列表当前显式为空（count=0 且无有效位），元素暂沿用通用词条结构，
    // 待属性数据解码后再细化。
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
    QolResult (*item_type)(QolItemHandle item, QolItemType* out_type);
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
    // Reserved ABI slot from the retired inventory extension provider accessor.
    void* reserved_3_7;
    QolResult (*count_limit)(QolItemHandle item, uint32_t* out_limit);
    QolResult (*count_encoding)(QolItemHandle item, QolItemCountEncoding* out_encoding);
    QolItemPortQuoteFn quote;
    // 空间化袋读语义（bag_count / bag_info / item_at / for_each_space_slot）已迁到
    // core.inventory 的 inventory.access v1.1；Item Port 只保留单点低层原语。
    // 3.8: serialize the complete stack via SAVE_SaveItem. A null out queries required size;
    // insufficient capacity returns InvalidArgument and still reports the required size.
    // Stale handles return NotFound; an unavailable game or symbol returns NotReady.
    QolResult (*serialize_item)(QolItemHandle item, uint8_t* out, uint32_t capacity,
                                uint32_t* out_size);
    // Frees a game-created item pointer that was not placed in native inventory.
    // Such an object has no QolItemHandle, so this is the only raw item pointer the
    // handle-only contract check allows (see tests/check_item_handle_contracts.cmake).
    QolResult (*free_item)(void* item);
    // 5.0：原生袋布局事实。插件侧空间语义（space 0/1）由 core.inventory 组合，
    // 模块只提供布局与低层单点读取。
    QolResult (*native_bag_layout)(QolNativeBagLayout* out);
    // 5.0：原生袋容量（INVEN_GetBagSize，按 space_id + bag 换算扁平袋号）。
    QolResult (*bag_capacity)(const QolBagRef* ref, int32_t* out_capacity);
    // 5.0：槽是否被占用（槽地址可读且物品指针非空）；不可读返回 NotReady。
    QolResult (*slot_occupied)(const QolBagRef* ref, int32_t slot, int32_t* out_occupied);
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

inline bool qol_item_port_native_bag_size_valid(const QolItemPort* port) {
    constexpr uint32_t kRequiredSize = static_cast<uint32_t>(
        offsetof(QolItemPort, read) + offsetof(QolItemPortReadApi, slot_occupied) +
        sizeof(QolItemPortReadApi::slot_occupied));
    return port != nullptr && port->struct_size >= kRequiredSize;
}

inline bool qol_item_port_transfer_size_valid(const QolItemPort* port) {
    constexpr uint32_t kRequiredSize = static_cast<uint32_t>(
        offsetof(QolItemPort, read) + offsetof(QolItemPortReadApi, serialize_item) +
        sizeof(decltype(QolItemPortReadApi::serialize_item)));
    return port != nullptr && port->struct_size >= kRequiredSize;
}

}  // namespace qol::contracts
