#pragma once

#include "contracts/bag.h"

namespace qol::contracts {
struct QolHostSdk;
}

namespace qol::features::core_item {

// core.item 不再拥有注册表；通过 inventory.bag_source 能力查询 core.inventory。
// bind 在主线程 install 时调用；能力暂不可用时查询返回空集合。
void bind_bag_source_sdk(const contracts::QolHostSdk* sdk);
// 能力撤销时只清缓存注册表指针，保留 host sdk 供下一次查询重新 require。
void invalidate_bag_source_registry();
// Host 测试用：直接注入注册表，绕过 capability 协商。
void bind_bag_source_registry_for_test(const contracts::QolBagSourceRegistryApi* api);
const contracts::QolBagSource* find_bag_source(int32_t space_id);
uint32_t registered_bag_source_count();
const contracts::QolBagSource* registered_bag_source_at(uint32_t index);
void reset_bag_sources();

}  // namespace qol::features::core_item
