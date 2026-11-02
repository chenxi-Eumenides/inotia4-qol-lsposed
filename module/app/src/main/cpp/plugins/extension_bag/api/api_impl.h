#pragma once

#include <string>

// 扩展物品使用语义：core.item 的 QolBagSource::use 调用此入口。
// 只返回插件原有业务结果信封；调用方按 QolResult 映射。
std::string extension_bag_api_use_item_impl(int logical_bag, int slot);
