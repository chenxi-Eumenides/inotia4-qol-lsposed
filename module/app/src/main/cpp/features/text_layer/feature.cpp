#include "features/text_layer/feature.h"

#include <atomic>
#include <cstdint>

#include "core/native/qol_log.h"
#include "hooks/dispatch.h"

namespace qol::features::text_layer {
namespace {

// 原函数 ABI（MEMORYTEXT_GetText；不引入符号头）。
using GetTextFn = const char* (*)(uint16_t);

// LSPosed native hook 写入的原始函数指针 backup（宿主经 on_resource_ready 交还）。
GetTextFn g_backup_memorytext_get_text = nullptr;

std::atomic<bool> g_active{false};

}  // namespace

void set_active(bool active) {
    g_active.store(active, std::memory_order_release);
}

bool active() {
    return g_active.load(std::memory_order_acquire);
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdGetText) == 0) {
        g_backup_memorytext_get_text = reinterpret_cast<GetTextFn>(backup);
        return 0;
    }
    QOL_LOG_WARN(QolDomain::kUi, "text_layer unknown resource id: %s", resource_id);
    return 0;
}

uint64_t api_subscribe(int32_t phase, int32_t kind, const char* (*fn)(uint32_t, const char*, void*),
                       void* ctx) {
    if (fn == nullptr) return 0;
    if (phase != static_cast<int32_t>(hooks::DispatchPhase::kBefore) &&
        phase != static_cast<int32_t>(hooks::DispatchPhase::kAfter)) {
        return 0;
    }
    if (kind != static_cast<int32_t>(hooks::ConsumerKind::kObserve) &&
        kind != static_cast<int32_t>(hooks::ConsumerKind::kModify) &&
        kind != static_cast<int32_t>(hooks::ConsumerKind::kIntercept)) {
        return 0;
    }
    return hooks::dispatch_subscribe(kResourceIdGetText,
                                     static_cast<hooks::DispatchPhase>(phase),
                                     static_cast<hooks::ConsumerKind>(kind), fn, ctx);
}

void api_unsubscribe(uint64_t id) {
    hooks::dispatch_unsubscribe(kResourceIdGetText, id);
}

const char* memorytext_get_text_wrapper(uint16_t text_id) {
    // 逐字对照旧 module_text wrapper：先无条件调原函数（保持原版行为与内部副作用），
    // 再按当前线程作用域查模块表；命中用模块字面量，否则用原值。
    const char* original =
        (g_backup_memorytext_get_text != nullptr) ? g_backup_memorytext_get_text(text_id) : nullptr;
    if (!g_active.load(std::memory_order_acquire)) return original;  // 撤销期/停用：透传原函数

    const char* owner_text = lookup(text_id, qol::infra::active_text_scope());
    const char* base = (owner_text != nullptr) ? owner_text : original;
    // 无消费者时 dispatch 直接返回 base，与旧行为逐字等价。
    return hooks::dispatch_run_text(kResourceIdGetText, text_id, base);
}

}  // namespace qol::features::text_layer
