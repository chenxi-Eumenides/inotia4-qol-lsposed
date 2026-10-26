#pragma once

#include "contracts/bag.h"

namespace qol::features::extension_bag {

// 扩展袋使用的注册空间。core.item 的注册表不分配 space_id，来源必须在第一次
// 注册时声明；当前 EB 占用第一个可注册空间（2）。
constexpr int32_t kExtensionBagSpaceId = qol::contracts::kQolBagSpaceFirstRegistered;

// 扩展袋的 QolBagSource 1.1 静态表（space=2）。所有回调只在游戏主线程调用。
const qol::contracts::QolBagSource* extension_bag_bag_source();

// core.item 的注册表由插件 activate 注入；重复注册返回 true。
bool extension_bag_bag_source_register(
    const qol::contracts::QolBagSourceRegistryApi* registry);

// deactivate / disable 时注销；注销后不得再回调本插件。
void extension_bag_bag_source_unregister();

bool extension_bag_bag_source_registered();

}  // namespace qol::features::extension_bag
