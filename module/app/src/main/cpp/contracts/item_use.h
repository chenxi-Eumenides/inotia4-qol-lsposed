#pragma once

// 物品使用后置通知契约（ARCHITECTURE.md §3.4 / §4.5）。
//
// `CHAR_UseItemEx` 的入口 hook 由 core.item 独占（一个资源只能有一个安装者）。
// 其他插件不重复 claim，也不直接调用原函数；它们通过本能力订阅“使用完成后”的通知，
// 再用自己的 game 访问层读取/修改需要的游戏状态。
//
// 事件只包含通用上下文，不包含任何冷却/倍率专用字段；后续新增用途不需要扩展本契约。

#include <cstdint>

#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kItemUseCapabilityId = "item.use";
constexpr QolVersion kItemUseVersion{1, 0};

// 一次 CHAR_UseItemEx 调用完成后的上下文快照。
// 只读：消费者不得通过事件写回任何字段。
struct QolItemUseEvent {
    uint32_t struct_size;

    void* character;  // 游戏角色指针；仅在回调期间可安全读取
    void* item;       // 被使用的物品指针；仅作标识，原函数可能已消耗/释放，禁止解引用
    int32_t flag;     // CHAR_UseItemEx 的第三个参数
    int32_t result;   // 原函数返回值；0 = 未使用/失败
};

using QolItemUsePostFn = void (*)(const QolItemUseEvent* event, void* ctx);

using QolItemUseSubscribeFn = QolResult (*)(const char* owner, QolItemUsePostFn fn,
                                            void* ctx, uint64_t* out_subscription);
using QolItemUseUnsubscribeFn = QolResult (*)(uint64_t subscription);

struct ItemUseApi {
    uint32_t struct_size;
    QolItemUseSubscribeFn subscribe_post;
    QolItemUseUnsubscribeFn unsubscribe_post;
};

inline bool item_use_api_size_valid(const ItemUseApi* api) {
    return api != nullptr && api->struct_size >= sizeof(ItemUseApi) &&
           api->subscribe_post != nullptr && api->unsubscribe_post != nullptr;
}

}  // namespace qol::contracts
