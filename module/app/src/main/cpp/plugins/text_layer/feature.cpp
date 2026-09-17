#include "plugins/text_layer/feature.h"

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

// 内置表消费者句柄（install 注册；0 = 未注册）。
hooks::DispatchId g_builtin_consumer = 0;

}  // namespace

const char* owner_table_modify(uint32_t text_id, const char* current, void* ctx) {
    (void)current;
    (void)ctx;
    // 命中返回模块字面量；未命中/占位 nullptr 返回 nullptr（不改）。
    return owner_table_lookup(text_id);
}

bool register_builtin_consumers() {
    if (g_builtin_consumer != 0) return true;  // 幂等（activate 重入）
    g_builtin_consumer = hooks::dispatch_subscribe(kResourceIdGetText, kOwnerTablePhase,
                                                   kOwnerTableKind, &owner_table_modify, nullptr);
    return g_builtin_consumer != 0;
}

void unregister_builtin_consumers() {
    if (g_builtin_consumer == 0) return;
    hooks::dispatch_unsubscribe(kResourceIdGetText, g_builtin_consumer);
    g_builtin_consumer = 0;
}

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
    // 先无条件调原函数（保持原版行为与内部副作用），得到**原始文本**。
    const char* original =
        (g_backup_memorytext_get_text != nullptr) ? g_backup_memorytext_get_text(text_id) : nullptr;
    if (!g_active.load(std::memory_order_acquire)) return original;  // 撤销期/停用：透传原函数
    // §4.5：值类型资源的 original 由 owner 在进入分派前计算（输入不可变），故该参数即
    // original 阶段的值；内置表作为 after 阶段 Modify 消费者随后生效。
    return hooks::dispatch_run_text(kResourceIdGetText, text_id, original);
}

}  // namespace qol::features::text_layer
