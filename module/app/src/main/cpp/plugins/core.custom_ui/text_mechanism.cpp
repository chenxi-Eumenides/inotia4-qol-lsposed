#include "text_mechanism.h"

#include <atomic>
#include <cstdint>

namespace qol::features::core_ui {
namespace {

// 原函数 ABI（MEMORYTEXT_GetText；不引入符号头）。
using GetTextFn = const char* (*)(uint16_t);

const contracts::QolHostSdk* g_sdk = nullptr;
// LSPosed native hook 写入的原始函数指针 backup（宿主经 on_resource_ready 交还）。
GetTextFn g_backup_memorytext_get_text = nullptr;
std::atomic<bool> g_active{false};

}  // namespace

void text_mechanism_bind(const contracts::QolHostSdk* sdk) { g_sdk = sdk; }

const contracts::QolHostSdk* text_mechanism_sdk() { return g_sdk; }

void text_set_active(bool active) { g_active.store(active, std::memory_order_release); }

bool text_active() { return g_active.load(std::memory_order_acquire); }

uint64_t text_api_subscribe(int32_t phase, int32_t kind,
                            const char* (*fn)(uint32_t, const char*, void*), void* ctx) {
    if (fn == nullptr) return 0;
    if (phase != 0 && phase != 1) {
        return 0;
    }
    if (kind < 0 || kind > 2) {
        return 0;
    }
    if (g_sdk == nullptr || g_sdk->hooks.text_subscribe == nullptr) return 0;
    uint64_t id = 0;
    const QolResult result = g_sdk->hooks.text_subscribe(
        kResourceIdGetText, phase, kind, reinterpret_cast<contracts::QolTextDispatchFn>(fn), ctx, &id);
    return contracts::succeeded(result) ? id : 0;
}

void text_api_unsubscribe(uint64_t id) {
    if (g_sdk != nullptr && g_sdk->hooks.text_unsubscribe != nullptr && id != 0) {
        (void)g_sdk->hooks.text_unsubscribe(kResourceIdGetText, id);
    }
}

void text_on_get_text_ready(void* backup) {
    g_backup_memorytext_get_text = reinterpret_cast<GetTextFn>(backup);
}

const char* memorytext_get_text_wrapper(uint16_t text_id) {
    // 先无条件调原函数（保持原版行为与内部副作用），得到**原始文本**。
    const char* original =
        (g_backup_memorytext_get_text != nullptr) ? g_backup_memorytext_get_text(text_id) : nullptr;
    if (!g_active.load(std::memory_order_acquire)) return original;  // 撤销期/停用：透传原函数
    // §4.5：值类型资源的 original 由 owner 在进入分派前计算（输入不可变），故该参数即
    // original 阶段的值；各内容插件的消费者随后按注册顺序生效。
    if (g_sdk == nullptr || g_sdk->hooks.run_text == nullptr) return original;
    const char* output = original;
    const QolResult result = g_sdk->hooks.run_text(kResourceIdGetText, text_id, original, &output);
    return contracts::succeeded(result) && output != nullptr ? output : original;
}

}  // namespace qol::features::core_ui
