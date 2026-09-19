#pragma once

#include <cstdint>

#include "contracts/host_sdk.h"
#include "contracts/game_host.h"

using NativeHookFunType = int (*)(void* func, void* replace, void** backup);
using NativeUnhookFunType = int (*)(void* func);

struct NativeAPIEntries {
    uint32_t version;
    NativeHookFunType hook_func;
    NativeUnhookFunType unhook_func;
};

using NativeOnModuleLoaded = void (*)(const char* name, void* handle);

// 使用 LSPosed Native Hook API 提供的 backup；扩展对象由逻辑库存适配器处理，原版对象回退到 backup。

void inventory_native_hook_on_api(const NativeAPIEntries* entries);
void inventory_native_hook_on_module_loaded(const char* name, void* handle);
// 模块主动刷新必须走 backup/trampoline，避免经被 Hook 地址回入 wrapper 后重复加锁。
// provider 实现（D2c-2b-1）：经 `inventory_refresh` capability 暴露给消费者；
// 返回 true = 已调原版（存在 trampoline），false = 无可用原函数。
bool inventory_native_hook_call_refresh_item_area_original();
bool inventory_native_hook_bind_sdk(const qol::contracts::QolHostSdk* sdk);
bool inventory_native_hook_bind_game(const qol::contracts::QolGameHost* game);

// 暴露框架原生 hook/unhook（native_init 时注入）；供扩展背包等功能按函数入口安装 hook。
// 过渡导出（清除阶段 P5）：机制原语迁入 patch/ 后删除。
NativeHookFunType native_hook_func();
NativeUnhookFunType native_unhook_func();

extern "C" [[gnu::visibility("default")]] [[gnu::used]]
NativeOnModuleLoaded native_init(const NativeAPIEntries* entries);

// ---------------------------------------------------------------------------
// S2 写侧进位/借位纯函数（R-45/R-49：写侧无统一 setter，各写点调用方完成类别
// 门控与 `count=128a+b` 回写；进位（b 溢出 127 → a+1）与借位（a>0 时 b 回绕
// → a-1、b+128）由 s2_write_count 编码内建）。
// 仅做位域算术、不触内存，供 wrapper 与后续 Host 断言使用。
// ---------------------------------------------------------------------------
namespace s2_writeback {

// original-only 写后确认：post 与 pre 不同、且恰好等于原版对 new_count 的 b 写
// 结果时，才认定「原版确实执行了该次数量写」。post == pre（含 delta≡0 mod 128
// 的退化与原版拒绝合并）一律 fail-closed 跳过，不得凭相等推断写发生。
inline bool original_count_write_confirmed(const qol::contracts::QolItemCountApi* api,
                                           uint32_t pre_field, uint32_t post_field,
                                           uint32_t new_count) {
    if (api == nullptr || api->canonical_from_field == nullptr ||
        api->field_from_canonical == nullptr || post_field == pre_field) {
        return false;
    }
    uint32_t pre_canonical = 0;
    uint32_t post_canonical = 0;
    uint32_t pre_without_b = 0;
    uint32_t post_without_b = 0;
    if (!qol::contracts::succeeded(api->canonical_from_field(pre_field, &pre_canonical)) ||
        !qol::contracts::succeeded(api->canonical_from_field(post_field, &post_canonical)) ||
        !qol::contracts::succeeded(api->field_from_canonical(pre_field, 0, 0, &pre_without_b)) ||
        !qol::contracts::succeeded(api->field_from_canonical(post_field, 0, 0,
                                                             &post_without_b))) {
        return false;
    }
    // field_from_canonical(..., 0, false) preserves the non-B field bits. The
    // canonical remainder identifies the original setter's seven-bit value
    // without duplicating its bit layout here.
    return pre_without_b == post_without_b &&
           (post_canonical % 128u) == (new_count % 128u);
}

// 加法回写全量：old_full + delta 后按模式上限收敛（启用 999 / 关闭 99，
// R-47 决策 b：old_full 必须是模式视图数量——启用态 128a+b、关闭态 b；
// 关闭态结果落在 b 域；facade 负责当前视图的上限收敛，delta 先按
// 编码上限收敛，避免极端入参溢出。
inline uint32_t added_count(const qol::contracts::QolItemCountApi* api,
                            uint32_t old_full, uint32_t delta) {
    if (api == nullptr || api->view_limit == nullptr) return old_full;
    uint32_t limit = 0;
    if (!qol::contracts::succeeded(api->view_limit(1, &limit))) return old_full;
    const uint32_t bounded = delta > limit ? limit : delta;
    return old_full + bounded;
}

// 减法回写全量：old_full - sub（调用方须保证 old_full > sub；old_full 为模式
// 视图数量，关闭态结果同样落在 b 域）。
inline uint32_t subtracted_count(uint32_t old_full, uint32_t sub) {
    return old_full > sub ? old_full - sub : 0u;
}

// 减 1 借位回写全量（old_full ≥ 2 时调用）。
inline uint32_t decremented_count(uint32_t old_full) {
    return old_full - 1u;
}

}  // namespace s2_writeback
