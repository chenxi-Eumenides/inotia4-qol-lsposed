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
// 转换被取消（选角放弃）：撤销 transitioning 占用；旧 session 已关闭不恢复，
// session 回到 Unavailable，不伪装恢复任何磁盘状态。
QolResult slot_data_abandon_transition();

// ---- 联合保存协调器专用面（save_coordinator；主线程）----
struct SlotDataSaveView {
    contracts::QolSlotSession session;
    uint64_t transaction;
    uint32_t count;
};
// 固定 ready session 的参与者集合并持引用；已有未决保存事务或 session 非 ready 拒绝。
QolResult slot_data_save_begin(uint64_t transaction, SlotDataSaveView* out);
// 逐个 capture（持引用、ctx.transaction=事务号）；枚举结束返回 kQolErrNotFound。
// 捕获跳过已 closing 的参与者（注销在途安全：引用未释放前回调已开始者完成本轮回调）。
QolResult slot_data_save_capture_next(uint64_t transaction, const char** section,
                                      uint16_t* version, uint8_t* buffer, uint64_t capacity,
                                      uint64_t* out_size);
// 终态：对未 closing 的参与者恰一次 complete（幂等内存记账）；outcome=kCommitted 时按
// 各自捕获快照清 dirty——捕获后又有新写者不误清。释放全部引用。
void slot_data_save_finish(uint64_t transaction, uint32_t outcome);

#if defined(QOL_SLOT_DATA_TEST_SEAMS)
// Host 测试缝：观察 SlotBound 参与者的 dirty 状态（验证 committed 按捕获 revision 清理）。
bool slot_data_dirty_for_test(contracts::QolSlotDataHandle handle);
#endif

}  // namespace qol::features::core_save
