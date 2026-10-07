#pragma once

#include "contracts/slot_data.h"

namespace qol::features::core_save {

// 私有装配入口；backend 可由 host tests 提供，不接入原版存档转换。
QolResult slot_data_initialize(contracts::QolOwnerToken provider_owner,
                              contracts::QolOwnerVerifyTokenFn verify,
                              const contracts::QolOwnerStoreApi* backend,
                              int32_t (*is_main_thread)());
const contracts::QolSlotDataApi& slot_data_api();
void slot_data_set_available(bool available);
// 由游戏主线程帧任务排空；注册调用栈绝不派发消费者回调。
void slot_data_pump();
// 转换桥仅在后续接线；测试驱动相同状态机，不调用原版保存。
QolResult slot_data_begin_transition(uint32_t reason, int32_t target);
QolResult slot_data_confirm_transition(int32_t current_slot);

}  // namespace qol::features::core_save
