#include "feature.h"

#include <atomic>
#include <cstdint>

#include "charinfo_scope.h"
#include "plugin.h"

namespace qol::features::text_layer {
namespace {

// 原函数 ABI（MEMORYTEXT_GetText；不引入符号头）。
using GetTextFn = const char* (*)(uint16_t);

// LSPosed native hook 写入的原始函数指针 backup（宿主经 on_resource_ready 交还）。
GetTextFn g_backup_memorytext_get_text = nullptr;

std::atomic<bool> g_active{false};
thread_local uint8_t g_active_scope = 0;

// 内置表消费者句柄（install 注册；0 = 未注册）。
uint64_t g_builtin_consumer = 0;

}  // namespace

const char* owner_table_modify(uint32_t text_id, const char* current, void* ctx) {
    (void)current;
    (void)ctx;
    // 命中返回模块字面量；未命中/占位 nullptr 返回 nullptr（不改）。
    return lookup(static_cast<uint16_t>(text_id),
                  static_cast<qol::infra::TextScope>(g_active_scope));
}

bool register_builtin_consumers() {
    if (g_builtin_consumer != 0) return true;  // 幂等（activate 重入）
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->hooks.text_subscribe == nullptr) return false;
    uint64_t id = 0;
    const QolResult result = sdk->hooks.text_subscribe(
        kResourceIdGetText, static_cast<int32_t>(kOwnerTablePhase),
        static_cast<int32_t>(kOwnerTableKind), &owner_table_modify, nullptr, &id);
    if (!contracts::succeeded(result)) return false;
    g_builtin_consumer = id;
    return id != 0;
}

void unregister_builtin_consumers() {
    if (g_builtin_consumer == 0) return;
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk != nullptr && sdk->hooks.text_unsubscribe != nullptr) {
        (void)sdk->hooks.text_unsubscribe(kResourceIdGetText, g_builtin_consumer);
    }
    g_builtin_consumer = 0;
}

void set_active(bool active) {
    g_active.store(active, std::memory_order_release);
}

bool active() {
    return g_active.load(std::memory_order_acquire);
}

uint8_t active_scope_value() { return g_active_scope; }

void set_active_scope_value(uint8_t value) { g_active_scope = value; }

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    (void)ctx;
    if (resource_id == nullptr) return 0;
    if (std::strcmp(resource_id, kResourceIdGetText) == 0) {
        g_backup_memorytext_get_text = reinterpret_cast<GetTextFn>(backup);
        return 0;
    }
    if (std::strcmp(resource_id, kResourceIdCharinfoScope) == 0) {
        return charinfo_scope_on_resource_ready(backup);
    }
    return 0;
}

uint64_t api_subscribe(int32_t phase, int32_t kind, const char* (*fn)(uint32_t, const char*, void*),
                       void* ctx) {
    if (fn == nullptr) return 0;
    if (phase != 0 && phase != 1) {
        return 0;
    }
    if (kind < 0 || kind > 2) {
        return 0;
    }
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->hooks.text_subscribe == nullptr) return 0;
    uint64_t id = 0;
    const QolResult result = sdk->hooks.text_subscribe(
        kResourceIdGetText, phase, kind,
        reinterpret_cast<contracts::QolTextDispatchFn>(fn), ctx, &id);
    return contracts::succeeded(result) ? id : 0;
}

void api_unsubscribe(uint64_t id) {
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk != nullptr && sdk->hooks.text_unsubscribe != nullptr && id != 0) {
        (void)sdk->hooks.text_unsubscribe(kResourceIdGetText, id);
    }
}

const char* memorytext_get_text_wrapper(uint16_t text_id) {
    // 先无条件调原函数（保持原版行为与内部副作用），得到**原始文本**。
    const char* original =
        (g_backup_memorytext_get_text != nullptr) ? g_backup_memorytext_get_text(text_id) : nullptr;
    if (!g_active.load(std::memory_order_acquire)) return original;  // 撤销期/停用：透传原函数
    // §4.5：值类型资源的 original 由 owner 在进入分派前计算（输入不可变），故该参数即
    // original 阶段的值；内置表作为 after 阶段 Modify 消费者随后生效。
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->hooks.run_text == nullptr) return original;
    const char* output = original;
    const QolResult result = sdk->hooks.run_text(kResourceIdGetText, text_id, original, &output);
    return contracts::succeeded(result) && output != nullptr ? output : original;
}

}  // namespace qol::features::text_layer
