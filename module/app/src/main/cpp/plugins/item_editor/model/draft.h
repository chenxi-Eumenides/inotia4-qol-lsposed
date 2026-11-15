#pragma once

#include "../../../contracts/game_ports/item_port.h"
#include "../../../contracts/item_type.h"

#include <cstdint>

namespace qol::features::item_editor {

// ITEMCLASSBASE 的 36 个类。类索引是类别编辑的唯一身份；具体物品 ID
//（ITEMDATABASE 下标）单独存放在 item_id，跨类时丢弃。
constexpr int32_t kItemClassCount = 36;
constexpr int32_t kItemClassGem = 25;
// 装备前缀 raw 2 是空前缀；新建装备/不可迁移时回到该值。
constexpr int32_t kDefaultRawRarity = 2;
constexpr int32_t kDefaultChaosRate = 100;

enum class ItemClassGroup : uint8_t {
    kWeapon,
    kEquipment,
    kItem,
    kUnknown,
};

ItemClassGroup item_class_group(int32_t class_index);
bool item_class_is_equipment(int32_t class_index);
bool item_class_valid(int32_t class_index);
contracts::QolItemType item_type_for_class(int32_t class_index);

struct ItemEditorDraft {
    contracts::QolItemHandle source{};
    bool source_valid = false;
    bool dirty = false;

    int32_t class_index = -1;  // ITEMCLASSBASE 0..35，-1 未选
    int32_t item_id = -1;      // ITEMDATABASE 下标，-1 已丢弃/未选
    contracts::QolItemType item_type = contracts::QolItemType::kUnknown;
    int32_t variant_series = 0;
    int32_t prefix_class_bits = 0;

    int32_t count = 1;
    bool stackable = true;
    int32_t raw_rarity = kDefaultRawRarity;
    int32_t socket_total = 0;
    int32_t socket_filled = 0;
    int32_t enchant_remaining = 0;
    int32_t enchant_id = 0;
    int32_t enchant_level = 0;
    int32_t enchant_increment = 0;
    int32_t enchant_threshold = 0;
    int32_t enchant_overage = 0;
    int32_t magic_rate = 0;
    int32_t chaos_level = 0;
    int32_t chaos_rate = kDefaultChaosRate;
    int32_t chaos_flags = 0;
    int32_t jewel_attribute_type = -1;
    int32_t jewel_value = 0;
    bool jewel_attribute_type_valid = false;

    // 派生值；类别变化后清零，等具体物品确定后重新读取/计算。
    int32_t damage = 0;
    int32_t defense = 0;
    int32_t need_level = 0;

    uint32_t native_option_count = 0;
    contracts::QolItemInventoryOptionView
        native_options[contracts::kQolItemInventoryNativeOptionCapacity]{};
    uint32_t jewel_option_count = 0;
    contracts::QolItemInventoryOptionView
        jewel_options[contracts::kQolItemInventoryJewelOptionCapacity]{};
    uint32_t enchant_option_count = 0;
    contracts::QolItemInventoryOptionView
        enchant_options[contracts::kQolItemInventoryEnchantOptionCapacity]{};
};

void draft_clear(ItemEditorDraft* draft);

// 从只读快照装载镜像；之后 UI 只读写 draft，不再读实际物品。
void draft_load(ItemEditorDraft* draft, contracts::QolItemHandle source,
                const contracts::QolItemInventoryView& view, int32_t class_index,
                contracts::QolItemType item_type);

// 切换类别。相同类别返回 false 且不改任何字段；跨类时按
// 装备族/物品族迁移，item_id 必定丢弃。
bool draft_change_class(ItemEditorDraft* draft, int32_t class_index);

bool draft_has_item(const ItemEditorDraft& draft);

}  // namespace qol::features::item_editor
