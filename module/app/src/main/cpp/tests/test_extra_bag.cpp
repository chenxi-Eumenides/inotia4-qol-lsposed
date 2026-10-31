#include "contracts/plugin.h"
#include "plugins/extra_bag/model/bag.h"
#include "plugins/extra_bag/model/item.h"
#include "plugins/extra_bag/model/item_backpack.h"
#include "plugins/extra_bag/model/store.h"
#include "plugins/extra_bag/plugin.h"

#include <cassert>
#include <cstring>

using qol::features::extra_bag::model::Bag;
using qol::features::extra_bag::model::ItemBackpackCapacity;
using qol::features::extra_bag::model::Store;

int main() {
    static_assert(Bag::kMaxCapacity == 16, "bag max capacity");
    static_assert(Store::kBagCount == 5, "default bag count");

    // 1. 默认 5 个背包；每包 capacity=0、无物品背包装备位、无普通物品。
    Store store;
    assert(store.bag_count() == 5);
    for (uint32_t i = 0; i < Store::kBagCount; ++i) {
        const Bag* bag = store.bag(i);
        assert(bag != nullptr);
        assert(bag->capacity() == 0);
        assert(!bag->has_item_backpack());
        assert(bag->item_backpack() == nullptr);
        assert(bag->item(0) == nullptr);
    }

    // 2. 容量档位：装备后 capacity 分别为 4/8/12/16；未装备=0；item_id 可读；
    //    卸下装备位恢复 0 且装备位指针为空。
    {
        Bag bag;
        assert(bag.capacity() == 0);
        assert(!bag.has_item_backpack());
        assert(bag.item_backpack() == nullptr);
        bag.equip_item_backpack({100, ItemBackpackCapacity::kFour});
        assert(bag.capacity() == 4);
        assert(bag.has_item_backpack());
        assert(bag.item_backpack() != nullptr);
        assert(bag.item_backpack()->item_id == 100);
        assert(bag.item_backpack()->capacity == ItemBackpackCapacity::kFour);
        bag.equip_item_backpack({101, ItemBackpackCapacity::kEight});
        assert(bag.capacity() == 8);
        assert(bag.item_backpack()->item_id == 101);
        bag.equip_item_backpack({102, ItemBackpackCapacity::kTwelve});
        assert(bag.capacity() == 12);
        assert(bag.item_backpack()->item_id == 102);
        bag.equip_item_backpack({103, ItemBackpackCapacity::kSixteen});
        assert(bag.capacity() == 16);
        assert(bag.item_backpack()->capacity == ItemBackpackCapacity::kSixteen);
        bag.unequip_item_backpack();
        assert(bag.capacity() == 0);
        assert(!bag.has_item_backpack());
        assert(bag.item_backpack() == nullptr);
    }

    // 3. 物品背包不参与 (bag_index, slot_index) 索引：装备后普通槽仍为空。
    {
        Store isolated;
        isolated.bag(0)->equip_item_backpack({7001, ItemBackpackCapacity::kSixteen});
        assert(isolated.bag(0)->item_backpack() != nullptr);
        assert(isolated.item(0, 0) == nullptr);
        assert(!isolated.take(0, 0).has_value());
    }

    // 4. 入库：直接写入对应 (bag_index, slot_index)，已存在则覆盖；
    //    不同背包的同 slot 互不影响。
    store.put(0, 3, {1001, 5});
    assert(store.item(0, 3) != nullptr);
    assert(store.item(0, 3)->item_id == 1001);
    assert(store.item(0, 3)->count == 5);
    assert(store.bag(0)->item(3) != nullptr);
    store.put(0, 3, {1002, 7});
    assert(store.item(0, 3)->item_id == 1002);
    assert(store.item(0, 3)->count == 7);
    store.put(1, 3, {2001, 9});
    assert(store.item(0, 3)->item_id == 1002);
    assert(store.item(1, 3)->item_id == 2001);

    // 5. 不做容量校验：capacity=0 或 4 时都允许写 slot 15。
    store.put(2, 15, {3001, 1});
    assert(store.item(2, 15) != nullptr);
    store.bag(2)->equip_item_backpack({9, ItemBackpackCapacity::kFour});
    assert(store.bag(2)->capacity() == 4);
    store.put(2, 15, {3002, 2});
    assert(store.item(2, 15)->item_id == 3002);

    // 6. 出库：存在则移除并返回；重复 take 或空槽返回 nullopt。
    const auto taken = store.take(0, 3);
    assert(taken.has_value());
    assert(taken->item_id == 1002);
    assert(taken->count == 7);
    assert(store.item(0, 3) == nullptr);
    assert(!store.take(0, 3).has_value());
    assert(!store.take(0, 99).has_value());
    assert(store.item(1, 3) != nullptr);

    // 7. bag_index 越界：put 静默无操作，take 返回 nullopt，item 返回 nullptr。
    store.put(99, 0, {9, 9});
    assert(store.item(99, 0) == nullptr);
    assert(!store.take(99, 0).has_value());
    assert(store.item(1, 3) != nullptr);

    // 8. clear 恢复 5 个空背包：物品与物品背包装备位一起清空。
    store.clear();
    assert(store.bag_count() == 5);
    for (uint32_t i = 0; i < Store::kBagCount; ++i) {
        const Bag* bag = store.bag(i);
        assert(bag != nullptr);
        assert(bag->capacity() == 0);
        assert(!bag->has_item_backpack());
        assert(bag->item_backpack() == nullptr);
        assert(bag->item(0) == nullptr);
    }

    // 9. plugin_descriptor 元数据与 install/deactivate 生命周期。
    const qol::contracts::PluginDescriptor& descriptor =
        qol::features::extra_bag::plugin_descriptor();
    assert(std::strcmp(descriptor.id, "extra_bag") == 0);
    assert(std::strcmp(descriptor.version, "0.1.0") == 0);
    assert(descriptor.feature_count == 1);
    assert(std::strcmp(descriptor.features[0].id, "extra_bag.enabled") == 0);
    assert(descriptor.features[0].category == qol::contracts::FeatureCategory::kHidden);
    assert(descriptor.features[0].type == qol::contracts::FeatureType::kToggle);
    assert(descriptor.features[0].default_enabled == 1);
    assert(descriptor.requires_count == 0 && descriptor.requires == nullptr);
    assert(descriptor.optional_count == 0 && descriptor.optional_requires == nullptr);
    assert(descriptor.provides_count == 0 && descriptor.provides == nullptr);
    assert(descriptor.resources_count == 0 && descriptor.resources == nullptr);

    qol::contracts::QolHostSdk sdk{};
    qol::contracts::PluginContext ctx{};
    ctx.struct_size = sizeof(ctx);
    ctx.sdk = &sdk;
    assert(descriptor.install(ctx) == 0);
    qol::contracts::PluginStatusOut status{};
    descriptor.fill_status(status);
    assert(status.enabled);
    assert(qol::features::extra_bag::model::store().bag_count() == Store::kBagCount);
    qol::features::extra_bag::model::store().put(0, 0, {1, 1});
    assert(qol::features::extra_bag::model::store().item(0, 0) != nullptr);
    descriptor.deactivate(ctx);
    descriptor.fill_status(status);
    assert(!status.enabled);
    assert(qol::features::extra_bag::model::store().bag_count() == Store::kBagCount);
    assert(qol::features::extra_bag::model::store().item(0, 0) == nullptr);

    // 无 sdk 时 install 返回 1。
    qol::contracts::PluginContext no_sdk{};
    no_sdk.struct_size = sizeof(no_sdk);
    no_sdk.sdk = nullptr;
    assert(descriptor.install(no_sdk) == 1);

    return 0;
}
