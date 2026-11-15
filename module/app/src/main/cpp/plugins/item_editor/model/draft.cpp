#include "draft.h"

#include <algorithm>

namespace qol::features::item_editor {
namespace {

void clear_options(ItemEditorDraft* draft) {
    draft->native_option_count = 0;
    draft->jewel_option_count = 0;
    draft->enchant_option_count = 0;
    for (auto& option : draft->native_options) option = {};
    for (auto& option : draft->jewel_options) option = {};
    for (auto& option : draft->enchant_options) option = {};
}

// 装备族可迁移数据回到新建装备的默认值。调用方决定是否保留旧装备数据。
void reset_equipment_fields(ItemEditorDraft* draft) {
    draft->raw_rarity = kDefaultRawRarity;
    draft->socket_total = 0;
    draft->socket_filled = 0;
    draft->enchant_remaining = 0;
    draft->enchant_id = 0;
    draft->enchant_level = 0;
    draft->enchant_increment = 0;
    draft->enchant_threshold = 0;
    draft->enchant_overage = 0;
    draft->magic_rate = 0;
    draft->chaos_level = 0;
    draft->chaos_rate = kDefaultChaosRate;
    draft->chaos_flags = 0;
    clear_options(draft);
}

void reset_jewel_fields(ItemEditorDraft* draft) {
    draft->jewel_attribute_type = -1;
    draft->jewel_value = 0;
    draft->jewel_attribute_type_valid = false;
}

template <size_t N>
void copy_options(contracts::QolItemInventoryOptionView (&destination)[N],
                  const contracts::QolItemInventoryOptionView (&source)[N], uint32_t source_count,
                  uint32_t* destination_count) {
    const uint32_t count = std::min<uint32_t>(source_count, static_cast<uint32_t>(N));
    for (uint32_t i = 0; i < count; ++i) destination[i] = source[i];
    *destination_count = count;
}

}  // namespace

ItemClassGroup item_class_group(int32_t class_index) {
    if (class_index >= 0 && class_index <= 8) return ItemClassGroup::kWeapon;
    // 9..21 包含布甲/皮甲/板甲、帽子/头盔、护手/鞋、盾牌、斗篷、项链/戒指、
    // 头环/黄金之冠。盾牌按装备槽位归入装备组。
    if (class_index >= 9 && class_index <= 21) return ItemClassGroup::kEquipment;
    if (class_index >= 22 && class_index < kItemClassCount) return ItemClassGroup::kItem;
    return ItemClassGroup::kUnknown;
}

bool item_class_is_equipment(int32_t class_index) {
    const ItemClassGroup group = item_class_group(class_index);
    return group == ItemClassGroup::kWeapon || group == ItemClassGroup::kEquipment;
}

bool item_class_valid(int32_t class_index) {
    return class_index >= 0 && class_index < kItemClassCount;
}

contracts::QolItemType item_type_for_class(int32_t class_index) {
    if (!item_class_valid(class_index)) return contracts::QolItemType::kUnknown;
    if (item_class_is_equipment(class_index)) return contracts::QolItemType::kEquipment;
    switch (class_index) {
        case 22:
        case 23: return contracts::QolItemType::kPotion;
        case 24: return contracts::QolItemType::kEnchantScroll;
        case 25: return contracts::QolItemType::kJewel;
        case 29: return contracts::QolItemType::kMercenarySeal;
        case 30: return contracts::QolItemType::kSkillBook;
        case 31: return contracts::QolItemType::kBackpack;
        case 32: return contracts::QolItemType::kDice;
        default: return contracts::QolItemType::kOther;
    }
}

void draft_clear(ItemEditorDraft* draft) {
    if (draft == nullptr) return;
    *draft = {};
    draft->item_type = contracts::QolItemType::kUnknown;
    draft->count = 1;
    draft->stackable = true;
    draft->raw_rarity = kDefaultRawRarity;
    draft->chaos_rate = kDefaultChaosRate;
    draft->jewel_attribute_type = -1;
}

void draft_load(ItemEditorDraft* draft, contracts::QolItemHandle source,
                const contracts::QolItemInventoryView& view, int32_t class_index,
                contracts::QolItemType item_type) {
    if (draft == nullptr) return;
    draft_clear(draft);
    draft->source = source;
    draft->source_valid = source.slot != 0 && source.generation != 0;
    draft->class_index = item_class_valid(class_index) ? class_index : -1;
    draft->stackable = view.count_supported != 0;
    const bool has_category = (view.fields & contracts::kQolItemInventoryHasCategory) != 0 ||
                              view.category > 0;
    draft->item_id = has_category && view.category >= 0 ? view.category : -1;
    draft->item_type = item_type;
    draft->variant_series = view.variant_series;
    draft->count = view.count_supported != 0 && view.effective_count > 0
        ? view.effective_count : 1;
    draft->raw_rarity = view.raw_rarity;
    draft->socket_total = view.socket_total;
    draft->socket_filled = view.socket_filled;
    draft->enchant_remaining = view.enchant_remaining;
    draft->enchant_id = view.enchant_id;
    draft->enchant_level = view.enchant_level;
    draft->magic_rate = view.magic_rate;
    draft->chaos_flags = view.chaos;
    // 混沌等级不参与编辑，draft 固定为 0；只保留混沌属性倍率。
    draft->chaos_level = 0;
    draft->chaos_rate = view.chaos_rate;
    draft->jewel_attribute_type = view.jewel_attribute_type;
    draft->jewel_value = view.jewel_value;
    draft->jewel_attribute_type_valid =
        (view.fields & contracts::kQolItemInventoryHasJewelAttributeType) != 0;
    draft->damage = view.damage;
    draft->defense = view.defense;
    draft->need_level = view.need_level;
    copy_options(draft->native_options, view.native_options, view.native_option_count,
                 &draft->native_option_count);
    copy_options(draft->jewel_options, view.jewel_options, view.jewel_option_count,
                 &draft->jewel_option_count);
    copy_options(draft->enchant_options, view.enchant_options, view.enchant_option_count,
                 &draft->enchant_option_count);
    draft->dirty = false;
}

bool draft_change_class(ItemEditorDraft* draft, int32_t class_index) {
    if (draft == nullptr || !item_class_valid(class_index)) return false;
    if (draft->class_index == class_index) return false;

    const int32_t old_class = draft->class_index;
    const ItemClassGroup old_group = item_class_group(old_class);
    const ItemClassGroup new_group = item_class_group(class_index);
    const bool old_equipment = old_group == ItemClassGroup::kWeapon ||
                               old_group == ItemClassGroup::kEquipment;
    const bool new_equipment = new_group == ItemClassGroup::kWeapon ||
                               new_group == ItemClassGroup::kEquipment;
    const int32_t kept_count = draft->count > 0 ? draft->count : 1;

    if (new_equipment) {
        // 装备族之间保留装备数据；从物品族进入装备族只保留空装备骨架。
        if (!old_equipment) reset_equipment_fields(draft);
        draft->count = 1;
        draft->stackable = false;
        reset_jewel_fields(draft);
        if (!old_equipment || old_group != new_group)
            draft->prefix_class_bits = new_group == ItemClassGroup::kWeapon ? 3 : 5;
    } else {
        // 物品族不保留装备专属数据；数量可迁移，宝石属性只在宝石类之间保留。
        reset_equipment_fields(draft);
        draft->count = kept_count;
        draft->stackable = true;
        const bool gem_to_gem = old_group == ItemClassGroup::kItem &&
                                new_group == ItemClassGroup::kItem &&
                                old_class == kItemClassGem && class_index == kItemClassGem;
        if (!gem_to_gem) reset_jewel_fields(draft);
        draft->prefix_class_bits = 0;
    }

    draft->class_index = class_index;
    // 具体 item_id 属于单一物品，换类后不能复用。
    draft->item_id = -1;
    draft->item_type = item_type_for_class(class_index);
    draft->damage = 0;
    draft->defense = 0;
    draft->need_level = 0;
    draft->dirty = true;
    return true;
}

bool draft_has_item(const ItemEditorDraft& draft) {
    return draft.item_id >= 0;
}

const char* enchant_strength_name(EnchantStrength strength) {
    switch (strength) {
        case EnchantStrength::kNone: return "无";
        case EnchantStrength::kLow: return "低级";
        case EnchantStrength::kMid: return "中级";
        case EnchantStrength::kHigh: return "高级";
        case EnchantStrength::kTop: return "顶级";
        case EnchantStrength::kSpecial: return "特级";
        case EnchantStrength::kChaos: return "混沌";
    }
    return "未知";
}

EnchantStrength enchant_strength_default(int32_t need_level) {
    if (need_level <= 33) return EnchantStrength::kLow;
    if (need_level <= 66) return EnchantStrength::kMid;
    if (need_level <= 99) return EnchantStrength::kHigh;
    return EnchantStrength::kTop;
}

namespace {

// 0 = 武器 class0；1 = 武器 class1..3；2 = 武器 class4..8；
// 3 = 防具 class9/12..15/17..21；4 = 防具 class10/16；5 = 防具 class11。
int32_t enchant_class_group(int32_t class_index) {
    if (class_index == 0) return 0;
    if (class_index >= 1 && class_index <= 3) return 1;
    if (class_index >= 4 && class_index <= 8) return 2;
    if (class_index == 9 || (class_index >= 12 && class_index <= 15) ||
        (class_index >= 17 && class_index <= 21)) return 3;
    if (class_index == 10 || class_index == 16) return 4;
    if (class_index == 11) return 5;
    return -1;
}

int32_t c18_enchant_id(int32_t row_index) {
    // C18 把 32 行表压缩进 4 位 ID；见 ITEMSYSTEM_EnchantItem 0x1077c0。
    if (row_index >= 0 && row_index <= 14) return row_index + 1;
    if (row_index >= 16 && row_index <= 30) return row_index - 15;
    return 0;
}

}  // namespace

int32_t enchant_id_for_strength(EnchantStrength strength, int32_t class_index,
                                int32_t variant_series) {
    if (strength == EnchantStrength::kNone) return 0;
    const int32_t group = enchant_class_group(class_index);
    if (group < 0) return 0;
    const int32_t tier = static_cast<int32_t>(strength) - 1;
    const int32_t row_index = group < 3 ? tier * 3 + group
                                        : 16 + tier * 3 + (group - 3);
    if (variant_series == 4) return c18_enchant_id(row_index);
    return row_index + 1;
}

EnchantStrength enchant_strength_for_id(int32_t enchant_id, int32_t class_index,
                                        int32_t variant_series) {
    if (enchant_id <= 0) return EnchantStrength::kNone;
    for (int32_t value = 1; value <= 6; ++value) {
        const auto strength = static_cast<EnchantStrength>(value);
        if (enchant_id_for_strength(strength, class_index, variant_series) == enchant_id)
            return strength;
    }
    return EnchantStrength::kNone;
}

}  // namespace qol::features::item_editor
