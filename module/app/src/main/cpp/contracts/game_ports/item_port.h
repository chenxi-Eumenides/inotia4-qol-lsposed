#pragma once

#include <cstdint>
#include <type_traits>

#include "../result.h"
#include "../version.h"
#include "../item_handle.h"

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

using QolInventoryBagSlotFn = int32_t (*)(const QolInventoryItemRef* item, void* ctx);
using QolItemJsonFn = QolResult (*)(char* out, uint32_t capacity, uint32_t* out_size);

constexpr uint32_t kQolItemInventoryOptionCapacity = 32;
// Autosell view consumer table (plugins/autosell/view.cpp): real-equip→equip; rarity→rarity;
// remaining-enchant→enchant_remaining; filled/total sockets→socket_filled/socket_total;
// jewel value/type→jewel_value/jewel_attribute_type; quantity uses effective_count when
// count_supported is set. Bag category remains QolInventoryItemRef.category. Consumers do not
// reinterpret payload or recompute the R-47b mode view.

enum QolItemInventoryViewField : uint32_t {
    kQolItemInventoryHasCategory = 1u << 0,
    kQolItemInventoryHasRarity = 1u << 1,
    kQolItemInventoryHasDamage = 1u << 2,
    kQolItemInventoryHasDefense = 1u << 3,
    kQolItemInventoryHasNeedLevel = 1u << 4,
    kQolItemInventoryHasJewelValue = 1u << 5,
    kQolItemInventoryHasJewelAttributeType = 1u << 6,
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
    QolResult (*detail_context)(int32_t* out_desc_type, int32_t* out_bag, void** out_control);
    QolResult (*cursor_index)(const void* control, int32_t* out_slot);
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
};

struct QolItemPort {
    QolVersion abi_version;
    uint32_t struct_size;
    QolItemPortReadApi read;
};

inline bool qol_item_port_size_valid(const QolItemPort* port) {
    return port != nullptr && port->struct_size >= sizeof(QolItemPort);
}

}  // namespace qol::contracts
