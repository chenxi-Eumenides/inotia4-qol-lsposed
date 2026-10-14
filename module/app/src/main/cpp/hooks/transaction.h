#pragma once

// 资源安装/还原事务引擎（ARCHITECTURE.md §3.2 生命周期、§4.2 物理归还、§4.4 事务表）。
//
// 状态机 + 帧步进：每次 tx_step 推进至多一个资源步骤，禁止主线程阻塞等待。
// 安装按注册表声明顺序，还原按逆序。执行期间只做机制调用、registry 读写与 gate
// 读取，不调用插件业务代码；同一插件同时只允许一个未终态事务。
//
// 机制原语由宿主注入（TxDeps），本模块不直接调用 LSPosed API。

#include <cstdint>

#include "contracts/plugin.h"
#include "hooks/install.h"

namespace qol::hooks {

enum class TxKind : uint8_t { kInstall = 0, kRelease = 1 };
enum class TxState : uint8_t { kRunning = 0, kSucceeded = 1, kFailed = 2 };

struct TxDeps {
    uintptr_t (*resolve_entry)(const char* symbol, bool* out_followed); // 必填
    NativeHookFn hook_fn;      // 必填
    NativeUnhookFn unhook_fn;  // 必填
    bool (*gate_on)(const char* gate_key);  // nullptr = 一切 gate 视为开

    // 非入口 hook 机制（如 kCallPatch）的物理安装/释放；由 host 注入（nullptr → 该机制不可用）。
    // hooks/ 不依赖 patch/：机制分派保持在宿主，本模块只按 mechanism 选择回调。
    struct MechanismOutcome {
        int32_t ok;
        const char* error;
    };
    MechanismOutcome (*install_other)(const contracts::ResourceClaim& claim, uintptr_t target) =
        nullptr;
    MechanismOutcome (*release_other)(const contracts::ResourceClaim& claim, uintptr_t target) =
        nullptr;
};
void tx_set_deps(const TxDeps& deps);   // 装配期一次；未设置时 tx_begin 返回 0

// 开始事务：按注册表声明顺序安装 / 逆序还原「owner == plugin_id」的全部资源。
// 返回 0 的拒绝条件：plugin_id 空 / deps 未设置 / 该插件无资源 / 同插件已有未终态事务 /
//   该插件任一资源 isolated==true（隔离后禁止重试，直到进程重启）。
uint64_t tx_begin(const char* plugin_id, TxKind kind);

struct TxStep {
    const char* resource_id = nullptr;  // 本步处理的资源（等待帧为 nullptr）
    int32_t ok = 0;                     // 本步结果（0=成功；非 0=本步失败；等待帧为 0）
    void* backup = nullptr;             // 安装成功时的原函数入口
    bool finished = false;              // 本次调用后事务是否终态（成功或失败）
    const char* error = nullptr;         // 本步失败原因；软失败也会保留在本字段
};
// 帧步进：推进至多一个资源步骤；返回 true = 仍需继续调用（含在途等待帧）。
bool tx_step(uint64_t tx_id, TxStep* out);

// 中止仍在途的安装事务：仅 Running 的 kInstall 可用；逆序回滚已装子集后置 Failed 并终态化。
// 用于「受限回调失败」路径——必须先终止安装，禁止后续步（尤其末尾 ownership acquire）继续。
// 返回 true = 本次确实中止；false = 事务不存在/非安装/已终态（调用方沿既有路径处理）。
bool tx_abort_install(uint64_t tx_id, const char* resource_id, const char* error);

struct TxStatus {
    TxKind kind = TxKind::kInstall;
    TxState state = TxState::kRunning;
    uint32_t step = 0;      // 已完成资源步骤数
    uint32_t total = 0;     // 资源总数
    uint32_t frames = 0;    // 已消耗帧数（每次 tx_step 计 1）
    uint32_t budget = 120;  // 当前资源的帧预算
    const char* failed_resource_id = nullptr;  // 静态字符串或 nullptr
    const char* error = nullptr;               // 静态字符串或 nullptr
};
bool tx_query(uint64_t tx_id, TxStatus* out);
void tx_reset_for_test();

}  // namespace qol::hooks
