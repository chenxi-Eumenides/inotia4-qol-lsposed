#include "contracts/plugin.h"
#include "plugins/extra_bag/model/bag.h"
#include "plugins/extra_bag/model/item_backpack.h"
#include "plugins/extra_bag/model/store.h"
#include "plugins/extra_bag/plugin.h"

#include <cassert>
#include <cstring>

using qol::features::extra_bag::model::Bag;
using qol::features::extra_bag::model::ItemBackpackCapacity;
using qol::features::extra_bag::model::Store;

int main() {
    // 1. 默认 5 个背包；每包 capacity=0、无物品背包装备位。
    static_assert(Bag::kMaxCapacity == 16, "bag max capacity");
    Store store;
    assert(store.bag_count() == 5);
    for (uint32_t i = 0; i < 5; ++i) {
        const Bag* bag = store.bag(i);
        assert(bag != nullptr);
        assert(bag->capacity() == 0);
        assert(!bag->has_item_backpack());
        assert(bag->item_backpack() == nullptr);
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

    // 3. 装备位独立：各背包装备位互不影响，capacity 只由自身装备决定。
    {
        assert(store.bag(0) != nullptr && store.bag(1) != nullptr && store.bag(2) != nullptr);
        store.bag(0)->equip_item_backpack({10, ItemBackpackCapacity::kFour});
        store.bag(1)->equip_item_backpack({11, ItemBackpackCapacity::kSixteen});
        assert(store.bag(0)->capacity() == 4);
        assert(store.bag(0)->item_backpack()->item_id == 10);
        assert(store.bag(1)->capacity() == 16);
        assert(store.bag(1)->item_backpack()->item_id == 11);
        assert(store.bag(2)->capacity() == 0);
        assert(!store.bag(2)->has_item_backpack());
        store.bag(0)->unequip_item_backpack();
        assert(store.bag(0)->capacity() == 0);
        assert(!store.bag(0)->has_item_backpack());
        assert(store.bag(1)->capacity() == 16);
        assert(store.bag(1)->has_item_backpack());
        store.clear();
    }

    // 4. set_bag_count：默认 5；设为 8、50 都生效（无硬上限）；设为 2 后只剩 2 个；
    //    clear 后恢复 5 个空背包（装备位一并清空）。
    {
        assert(store.bag_count() == 5);
        store.set_bag_count(8);
        assert(store.bag_count() == 8);
        assert(store.bag(7) != nullptr);
        store.set_bag_count(50);
        assert(store.bag_count() == 50);
        assert(store.bag(49) != nullptr);
        assert(store.bag(50) == nullptr);
        store.set_bag_count(2);
        assert(store.bag_count() == 2);
        assert(store.bag(1) != nullptr);
        assert(store.bag(2) == nullptr);
        store.bag(0)->equip_item_backpack({9, ItemBackpackCapacity::kTwelve});
        store.clear();
        assert(store.bag_count() == 5);
        for (uint32_t i = 0; i < 5; ++i) {
            assert(store.bag(i) != nullptr);
            assert(store.bag(i)->capacity() == 0);
            assert(!store.bag(i)->has_item_backpack());
            assert(store.bag(i)->item_backpack() == nullptr);
        }
    }

    // 5. plugin_descriptor 元数据与 install/deactivate 生命周期（静态面：包数量与装备位清空）。
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
    assert(qol::features::extra_bag::model::store().bag_count() == 5);
    qol::features::extra_bag::model::store().bag(0)->equip_item_backpack(
        {1, ItemBackpackCapacity::kEight});
    descriptor.deactivate(ctx);
    descriptor.fill_status(status);
    assert(!status.enabled);
    assert(qol::features::extra_bag::model::store().bag_count() == 5);
    assert(qol::features::extra_bag::model::store().bag(0) != nullptr);
    assert(qol::features::extra_bag::model::store().bag(0)->capacity() == 0);
    assert(!qol::features::extra_bag::model::store().bag(0)->has_item_backpack());

    // 无 sdk 时 install 返回 1。
    qol::contracts::PluginContext no_sdk{};
    no_sdk.struct_size = sizeof(no_sdk);
    no_sdk.sdk = nullptr;
    assert(descriptor.install(no_sdk) == 1);

    return 0;
}
