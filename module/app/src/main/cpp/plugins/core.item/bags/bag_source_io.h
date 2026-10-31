#pragma once

// 内部袋源接口：新编排层只依赖本接口，不依赖具体来源实现。
// 原版袋源与注册袋源适配器分别实现；所有回调必须在游戏主线程调用。
//
// 独立边界：
// - plugins/core.item/orchestration/ 只包含本头与自身头文件；
// - plugins/core.item/native_source/ 只包含本头与 contracts/ 头文件；
// - 两者互不 include，由未来的装配点（runtime/lifecycle.cpp 或 plugin.cpp）连接。

#include <cstdint>

#include "contracts/bag.h"

namespace qol::features::core_item::bags {

class BagSourceIo {
public:
    virtual ~BagSourceIo() = default;

    // 读面。
    virtual int32_t space_id() const = 0;
    virtual int32_t count() const = 0;
    virtual QolResult info(int32_t bag, contracts::QolBagInfo* out) const = 0;
    virtual QolResult item_at(int32_t bag, int32_t slot,
                              contracts::QolInventoryItemRef* out) const = 0;
    virtual QolResult for_each_item(contracts::QolInventoryBagSlotFn fn, void* ctx) const = 0;

    // 同空间移动：原版袋实现委托 Host move_item；注册源实现决定使用
    // export/import/remove 还是自身原语。
    virtual QolResult move_within(const contracts::QolBagRef& from, int32_t from_slot,
                                  int32_t count, const contracts::QolBagRef& to, int32_t to_slot,
                                  contracts::QolBagMutationResult* out) = 0;

    // 跨空间转移原语：导出只读；导入返回实际位置；移除按预期句柄执行。
    virtual QolResult export_item(contracts::QolItemHandle item, int32_t count,
                                  contracts::QolBagTransfer* out) const = 0;
    virtual QolResult import_item(const contracts::QolBagTransfer& transfer,
                                  int32_t preferred_bag, int32_t preferred_slot,
                                  contracts::QolBagMutationResult* out) = 0;
    virtual QolResult remove(contracts::QolItemHandle item, int32_t count,
                             contracts::QolBagMutationResult* out) = 0;

    // 可选面：缺失时返回 kQolErrUnsupported。
    virtual QolResult identify(contracts::QolItemHandle item, contracts::QolBagRef* out,
                               int32_t* out_slot) const {
        (void)item;
        (void)out;
        (void)out_slot;
        return contracts::error(kQolErrUnsupported, "identify unavailable",
                                sizeof("identify unavailable") - 1);
    }
    virtual QolResult use(contracts::QolBagRef ref, int32_t slot) {
        (void)ref;
        (void)slot;
        return contracts::error(kQolErrUnsupported, "use unavailable",
                                sizeof("use unavailable") - 1);
    }
};

}  // namespace qol::features::core_item::bags
