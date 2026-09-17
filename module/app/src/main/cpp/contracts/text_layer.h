#pragma once

#include <cstdint>

// 文本层能力契约（ARCHITECTURE.md §3.4、§4.5）：共享资源 MEMORYTEXT_GetText 的消费者入口。
// 只读/回调登记接口；提供方保证在撤销事务完成前有效。

namespace qol::contracts {

constexpr const char* kTextLayerCapabilityId = "text_layer";
constexpr int32_t kTextLayerVersionMajor = 1;
constexpr int32_t kTextLayerVersionMinor = 0;

// phase 取 hooks::DispatchPhase 数值、kind 取 hooks::ConsumerKind 数值
// （contracts 不依赖 hooks）；返回 0 = 注册失败。
using TextSubscribeFn = uint64_t (*)(int32_t phase, int32_t kind,
                                     const char* (*fn)(uint32_t, const char*, void*), void* ctx);
using TextUnsubscribeFn = void (*)(uint64_t id);

struct TextLayerApi {   // POD：只读查询 / 消费者登记（§3.4 类型限制）
    TextSubscribeFn subscribe;
    TextUnsubscribeFn unsubscribe;
};

}  // namespace qol::contracts
