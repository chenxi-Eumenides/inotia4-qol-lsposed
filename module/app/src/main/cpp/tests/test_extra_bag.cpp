#include "contracts/extra_bag.h"
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
using qol::features::extra_bag::model::RuntimeItem;
using qol::features::extra_bag::model::Store;

namespace {

RuntimeItem make_item(int32_t category, int32_t count) {
    RuntimeItem item{};
    item.category = category;
    item.count = count;
    return item;
}

RuntimeItem make_item_with_payload(int32_t category, int32_t count, const char* payload) {
    RuntimeItem item = make_item(category, count);
    const size_t size = std::strlen(payload);
    assert(size <= RuntimeItem::kPayloadMaxSize);
    std::memcpy(item.payload, payload, size);
    item.payload_size = static_cast<uint32_t>(size);
    return item;
}

qol::contracts::QolExtraBagItem make_transfer_item(int32_t category, int32_t count,
                                                   const char* payload) {
    qol::contracts::QolExtraBagItem item{};
    item.category = category;
    item.count = count;
    if (payload != nullptr) {
        const size_t size = std::strlen(payload);
        assert(size <= qol::contracts::kExtraBagPayloadMaxSize);
        std::memcpy(item.payload, payload, size);
        item.payload_size = static_cast<uint32_t>(size);
    }
    return item;
}

}  // namespace

int main() {
    static_assert(Bag::kMaxCapacity == 16, "bag max capacity");
    static_assert(Store::kBagCount == 5, "default bag count");
    static_assert(RuntimeItem::kPayloadCapacity == 256, "payload capacity");

    // 1. 默认 5 个背包；每包 capacity=0、无物品背包装备位、无普通物品。
    Store store;
    assert(store.bag_count() == 5);
    for (uint32_t i = 0; i < Store::kBagCount; ++i) {
        const Bag* bag = store.bag(i);
        assert(bag != nullptr);
        assert(bag->capacity() == 0);
        assert(!bag->has_item_backpack());
        assert(bag->item_backpack() == nullptr);
        assert(bag->occupied_slots() == 0);
        assert(bag->item(0) == nullptr);
    }

    // 2. 容量档位：装备后 capacity 分别为 4/8/12/16；未装备=0；物品数据可读；
    //    卸下装备位恢复 0 且装备位指针为空。
    {
        Bag bag;
        assert(bag.capacity() == 0);
        assert(!bag.has_item_backpack());
        assert(bag.item_backpack() == nullptr);
        bag.equip_item_backpack({make_item(100, 1), ItemBackpackCapacity::kFour});
        assert(bag.capacity() == 4);
        assert(bag.has_item_backpack());
        assert(bag.item_backpack() != nullptr);
        assert(bag.item_backpack()->item.category == 100);
        assert(bag.item_backpack()->capacity == ItemBackpackCapacity::kFour);
        bag.equip_item_backpack({make_item(101, 1), ItemBackpackCapacity::kEight});
        assert(bag.capacity() == 8);
        assert(bag.item_backpack()->item.category == 101);
        bag.equip_item_backpack({make_item(102, 1), ItemBackpackCapacity::kTwelve});
        assert(bag.capacity() == 12);
        assert(bag.item_backpack()->item.category == 102);
        bag.equip_item_backpack({make_item(103, 1), ItemBackpackCapacity::kSixteen});
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
        isolated.bag(0)->equip_item_backpack({make_item(7001, 1), ItemBackpackCapacity::kSixteen});
        assert(isolated.bag(0)->item_backpack() != nullptr);
        assert(isolated.item(0, 0) == nullptr);
        assert(!isolated.take(0, 0).has_value());
    }

    // 4. 入库：直接写入对应 (bag_index, slot_index)，已存在则覆盖；
    //    不同背包的同 slot 互不影响。
    store.put(0, 3, make_item(1001, 5));
    assert(store.item(0, 3) != nullptr);
    assert(store.item(0, 3)->category == 1001);
    assert(store.item(0, 3)->count == 5);
    assert(store.bag(0)->item(3) != nullptr);
    assert(store.bag(0)->occupied_slots() == 1);
    store.put(0, 3, make_item(1002, 7));
    assert(store.item(0, 3)->category == 1002);
    assert(store.item(0, 3)->count == 7);
    store.put(1, 3, make_item(2001, 9));
    assert(store.item(0, 3)->category == 1002);
    assert(store.item(1, 3)->category == 2001);

    // 5. payload 随物品一起存取，长度与内容保持不变。
    store.put(1, 2, make_item_with_payload(2002, 3, "payload-abc"));
    assert(store.item(1, 2) != nullptr);
    assert(store.item(1, 2)->payload_size == 11);
    assert(std::memcmp(store.item(1, 2)->payload, "payload-abc", 11) == 0);

    // 6. 不做容量校验：capacity=0 或 4 时都允许写 slot 15。
    store.put(2, 15, make_item(3001, 1));
    assert(store.item(2, 15) != nullptr);
    store.bag(2)->equip_item_backpack({make_item(9, 1), ItemBackpackCapacity::kFour});
    assert(store.bag(2)->capacity() == 4);
    store.put(2, 15, make_item(3002, 2));
    assert(store.item(2, 15)->category == 3002);

    // 7. 出库：存在则移除并返回；重复 take 或空槽返回 nullopt。
    const auto taken = store.take(0, 3);
    assert(taken.has_value());
    assert(taken->category == 1002);
    assert(taken->count == 7);
    assert(store.item(0, 3) == nullptr);
    assert(!store.take(0, 3).has_value());
    assert(!store.take(0, 99).has_value());
    assert(store.item(1, 3) != nullptr);

    // 8. bag_index 越界：put 静默无操作，take 返回 nullopt，item 返回 nullptr。
    store.put(99, 0, make_item(9, 9));
    assert(store.item(99, 0) == nullptr);
    assert(!store.take(99, 0).has_value());
    assert(store.item(1, 3) != nullptr);

    // 9. clear 恢复 5 个空背包：物品与物品背包装备位一起清空。
    store.clear();
    assert(store.bag_count() == 5);
    for (uint32_t i = 0; i < Store::kBagCount; ++i) {
        const Bag* bag = store.bag(i);
        assert(bag != nullptr);
        assert(bag->capacity() == 0);
        assert(!bag->has_item_backpack());
        assert(bag->item_backpack() == nullptr);
        assert(bag->item(0) == nullptr);
        assert(bag->occupied_slots() == 0);
    }

    // 10. plugin_descriptor 元数据与 install 生命周期。
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
    assert(descriptor.provides_count == 1 && descriptor.provides != nullptr);
    assert(std::strcmp(descriptor.provides[0].id, qol::contracts::kExtraBagCapabilityId) == 0);
    assert(descriptor.provides[0].version.major == qol::contracts::kExtraBagVersionMajor);
    assert(descriptor.provides[0].version.minor == qol::contracts::kExtraBagVersionMinor);
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

    // 11. 公共接口：通过 capability 函数表完成入库/读取/出库。
    const auto* api = static_cast<const qol::contracts::QolExtraBagApi*>(
        descriptor.provides[0].iface);
    assert(api != nullptr);
    assert(api->struct_size == sizeof(qol::contracts::QolExtraBagApi));
    assert(api->bag_count != nullptr && api->bag_info != nullptr && api->item_at != nullptr &&
           api->put != nullptr && api->take != nullptr);
    assert(api->bag_count() == 5);

    qol::contracts::QolExtraBagInfo info{};
    api->bag_info(0, &info);
    assert(info.capacity == 0);
    assert(info.occupied_slots == 0);

    qol::features::extra_bag::model::store().bag(0)->equip_item_backpack(
        {make_item(7001, 1), ItemBackpackCapacity::kFour});
    api->bag_info(0, &info);
    assert(info.capacity == 4);
    assert(info.occupied_slots == 0);

    const qol::contracts::QolExtraBagItem item = make_transfer_item(1001, 5, "abc");
    api->put(0, 3, &item);
    qol::contracts::QolExtraBagItem out{};
    assert(api->item_at(0, 3, &out));
    assert(out.category == 1001);
    assert(out.count == 5);
    assert(out.payload_size == 3);
    assert(std::memcmp(out.payload, "abc", 3) == 0);
    api->bag_info(0, &info);
    assert(info.occupied_slots == 1);

    const qol::contracts::QolExtraBagItem replacement = make_transfer_item(1002, 7, "wxyz");
    api->put(0, 3, &replacement);
    assert(api->item_at(0, 3, &out));
    assert(out.category == 1002);
    assert(out.count == 7);
    assert(out.payload_size == 4);
    assert(std::memcmp(out.payload, "wxyz", 4) == 0);

    assert(api->take(0, 3, &out));
    assert(out.category == 1002);
    assert(out.count == 7);
    assert(!api->item_at(0, 3, &out));
    assert(!api->take(0, 3, &out));

    // 12. 不做校验：零值物品也直接入库；越界 bag 静默为空。
    const qol::contracts::QolExtraBagItem zero{};
    api->put(1, 0, &zero);
    assert(api->item_at(1, 0, &out));
    assert(out.category == 0);
    assert(out.count == 0);
    assert(out.payload_size == 0);
    api->put(-1, 0, &item);
    assert(!api->item_at(-1, 0, &out));
    assert(!api->take(-1, 0, &out));
    qol::contracts::QolExtraBagInfo invalid{1, 1};
    api->bag_info(99, &invalid);
    assert(invalid.capacity == 0);
    assert(invalid.occupied_slots == 0);

    // 13. 物品背包不参与普通槽索引。
    assert(!api->item_at(0, 0, &out));
    assert(!api->take(0, 0, &out));

    descriptor.deactivate(ctx);
    descriptor.fill_status(status);
    assert(!status.enabled);
    assert(qol::features::extra_bag::model::store().bag_count() == Store::kBagCount);
    assert(qol::features::extra_bag::model::store().item(0, 3) == nullptr);

    // 无 sdk 时 install 返回 1。
    qol::contracts::PluginContext no_sdk{};
    no_sdk.struct_size = sizeof(no_sdk);
    no_sdk.sdk = nullptr;
    assert(descriptor.install(no_sdk) == 1);

    return 0;
}
