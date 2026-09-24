#pragma once

#include "game_host.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kStackLimitItemCountCapabilityId = "stack_limit.item_count";
constexpr QolVersion kStackLimitItemCountVersion{1, 0};

struct StackLimitItemCountCapability {
    const QolItemCountApi* api;
    int32_t (*enabled)();
};

// 宿主侧遗留消费者的只读访问器；provider 未驻留时返回 nullptr。
// api/native 与 core/native 的旧调用者本轮保留此入口及原行为；其清退随阶段 6 的
// api/native 删除批进行。插件消费者应通过 Host SDK 获取 item.facts / stack_limit capability。
const QolItemCountApi* item_count_api();

}  // namespace qol::contracts
