#include "plugins/item_editor/model/draft.h"

#include <cstdio>

using namespace qol::contracts;
using namespace qol::features::item_editor;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

static QolItemInventoryView equipment_view() {
    QolItemInventoryView view{};
    view.category = 123;
    view.count_supported = 0;
    view.effective_count = 1;
    view.raw_rarity = 12;
    view.socket_total = 3;
    view.socket_filled = 1;
    view.enchant_remaining = 4;
    view.enchant_id = 29;
    view.enchant_level = 7;
    view.magic_rate = 110;
    view.chaos = 3;
    view.chaos_level = 2;
    view.chaos_rate = 85;
    view.damage = 10;
    view.defense = 20;
    view.need_level = 5;
    view.native_option_count = 2;
    view.native_options[0] = {11, 3, 0};
    view.native_options[1] = {22, 4, 0};
    view.jewel_option_count = 1;
    view.jewel_options[0] = {33, 5, 1};
    view.enchant_option_count = 1;
    view.enchant_options[0] = {44, 29, 2};
    return view;
}

static QolItemInventoryView item_view() {
    QolItemInventoryView view{};
    view.category = 5;
    view.count_supported = 1;
    view.effective_count = 7;
    view.raw_rarity = 0;
    view.native_option_count = 1;
    view.native_options[0] = {99, 1, 0};
    return view;
}

int main() {
    CHECK(item_class_valid(0));
    CHECK(item_class_valid(35));
    CHECK(!item_class_valid(-1));
    CHECK(!item_class_valid(36));
    CHECK(item_class_group(0) == ItemClassGroup::kWeapon);
    CHECK(item_class_group(8) == ItemClassGroup::kWeapon);
    CHECK(item_class_group(9) == ItemClassGroup::kEquipment);
    CHECK(item_class_group(16) == ItemClassGroup::kEquipment);  // 盾牌
    CHECK(item_class_group(21) == ItemClassGroup::kEquipment);
    CHECK(item_class_group(22) == ItemClassGroup::kItem);
    CHECK(item_class_group(35) == ItemClassGroup::kItem);
    CHECK(item_class_is_equipment(0));
    CHECK(item_class_is_equipment(16));
    CHECK(!item_class_is_equipment(22));

    ItemEditorDraft draft{};
    draft_clear(&draft);
    CHECK(draft.class_index == -1);
    CHECK(draft.item_id == -1);
    CHECK(draft.count == 1);
    CHECK(draft.raw_rarity == kDefaultRawRarity);
    CHECK(draft.chaos_rate == kDefaultChaosRate);
    CHECK(draft.jewel_attribute_type == -1);

    const QolItemHandle source{12, 3};
    draft_load(&draft, source, equipment_view(), 7, QolItemType::kEquipment);
    CHECK(draft.source_valid);
    CHECK(draft.class_index == 7);
    CHECK(draft.item_id == 123);
    CHECK(draft.item_type == QolItemType::kEquipment);
    CHECK(draft.count == 1);
    CHECK(draft.raw_rarity == 12);
    CHECK(draft.socket_total == 3 && draft.socket_filled == 1);
    CHECK(draft.enchant_remaining == 4 && draft.enchant_id == 29 && draft.enchant_level == 7);
    CHECK(draft.magic_rate == 110);
    CHECK(draft.chaos_flags == 3);
    CHECK(draft.chaos_level == 2 && draft.chaos_rate == 85);
    CHECK(draft.native_option_count == 2);
    CHECK(draft.jewel_option_count == 1);
    CHECK(draft.enchant_option_count == 1);
    CHECK(!draft.dirty);

    // 同类确认 == 取消：不迁移、不清 item_id。
    CHECK(!draft_change_class(&draft, 7));
    CHECK(draft.item_id == 123);
    CHECK(!draft.dirty);

    // 装备 -> 装备：保留装备数据，丢弃 item_id。
    CHECK(draft_change_class(&draft, 0));
    CHECK(draft.class_index == 0);
    CHECK(draft.item_id == -1);
    CHECK(!draft_has_item(draft));
    CHECK(draft.dirty);
    CHECK(draft.raw_rarity == 12);
    CHECK(draft.socket_total == 3 && draft.socket_filled == 1);
    CHECK(draft.enchant_remaining == 4 && draft.enchant_id == 29 && draft.enchant_level == 7);
    CHECK(draft.magic_rate == 110);
    CHECK(draft.chaos_flags == 3);
    CHECK(draft.chaos_level == 2 && draft.chaos_rate == 85);
    CHECK(draft.native_option_count == 2 && draft.jewel_option_count == 1 &&
          draft.enchant_option_count == 1);
    CHECK(draft.jewel_attribute_type == -1 && draft.jewel_value == 0);

    // 装备 -> 物品：只保留数量，装备数据全部丢弃。
    draft_load(&draft, source, equipment_view(), 7, QolItemType::kEquipment);
    draft.count = 5;
    CHECK(draft_change_class(&draft, 22));
    CHECK(draft.class_index == 22);
    CHECK(draft.item_id == -1);
    CHECK(draft.count == 5);
    CHECK(draft.raw_rarity == kDefaultRawRarity);
    CHECK(draft.socket_total == 0 && draft.socket_filled == 0);
    CHECK(draft.enchant_remaining == 0 && draft.enchant_id == 0 && draft.enchant_level == 0);
    CHECK(draft.magic_rate == 0);
    CHECK(draft.chaos_level == 0 && draft.chaos_rate == kDefaultChaosRate);
    CHECK(draft.native_option_count == 0 && draft.jewel_option_count == 0 &&
          draft.enchant_option_count == 0);
    CHECK(draft.jewel_attribute_type == -1 && draft.jewel_value == 0);

    // 物品 -> 物品：保留数量，丢弃装备数据和 item_id。
    draft_load(&draft, source, item_view(), 22, QolItemType::kPotion);
    CHECK(draft.item_id == 5);
    CHECK(draft.count == 7);
    CHECK(draft_change_class(&draft, 24));
    CHECK(draft.class_index == 24);
    CHECK(draft.item_id == -1);
    CHECK(draft.count == 7);
    CHECK(draft.native_option_count == 0);
    CHECK(draft.jewel_attribute_type == -1 && draft.jewel_value == 0);

    // 物品 -> 装备：空装备骨架，数量 1，装备默认值。
    draft_load(&draft, source, item_view(), 22, QolItemType::kPotion);
    CHECK(draft_change_class(&draft, 0));
    CHECK(draft.class_index == 0);
    CHECK(draft.item_id == -1);
    CHECK(draft.count == 1);
    CHECK(draft.raw_rarity == kDefaultRawRarity);
    CHECK(draft.socket_total == 0);
    CHECK(draft.chaos_rate == kDefaultChaosRate);
    CHECK(draft.native_option_count == 0);

    // 宝石同类确认保留宝石属性；宝石 -> 非宝石物品丢弃。
    QolItemInventoryView gem = item_view();
    gem.category = 30;
    gem.count_supported = 1;
    gem.effective_count = 1;
    gem.jewel_attribute_type = 3;
    gem.jewel_value = 60;
    draft_load(&draft, source, gem, kItemClassGem, QolItemType::kJewel);
    CHECK(!draft_change_class(&draft, kItemClassGem));
    CHECK(draft.jewel_attribute_type == 3 && draft.jewel_value == 60);
    CHECK(draft.item_id == 30);
    CHECK(draft_change_class(&draft, 24));
    CHECK(draft.jewel_attribute_type == -1 && draft.jewel_value == 0);

    CHECK(!draft_change_class(&draft, -1));
    CHECK(!draft_change_class(&draft, kItemClassCount));
    CHECK(!draft_change_class(nullptr, 0));

    std::printf("item_editor_draft_tests pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
