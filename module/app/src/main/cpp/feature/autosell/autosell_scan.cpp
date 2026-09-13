// autosell_scan.cpp —— 自动出售扫描与主线程逐帧 tick（阶段 B）。
//
// 扫描范围：原版物理袋 0..4（for_each_bag_slot，排除任务袋 5）+ 扩展逻辑袋 5 个
// （6..10，经稳定端口 inventory_item_ref_at 逐槽读取，每次调用自行取放锁；
// 不在 extension_bag_for_each_logical_item 回调内处置）。
// 处置：autosell_build_view -> should_sell -> NoSell 预过滤
//       -> inventory_trade::sell(bag, slot, ref.native_item, "autosell")（I-1 身份复核）。

#include "feature/autosell/autosell_scan.h"

#include "core/native/extension_bag_port.h"
#include "core/native/frame_task.h"
#include "core/native/inventory_trade.h"
#include "core/native/save_enter.h"
#include "core/native/save_exit.h"
#include "data/native/game_symbols.h"
#include "feature/autosell/autosell_config.h"
#include "feature/autosell/autosell_store.h"
#include "feature/autosell/autosell_view.h"
#include "game_access.h"
#include "game_state.h"

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace {

constexpr char kTag[] = "Inotia4AutoSell";
constexpr int kLastPhysicalBag = 4;      // 原版任务袋 5 排除
constexpr int kLogicalBagScanFirst = 6;  // = kExtensionLogicalBagFirst
constexpr int kLogicalBagScanCount = 5;  // = kBagCount
constexpr int kSlotCount = 16;

std::mutex g_scan_mtx;
std::mutex g_task_mtx;
FrameTaskId g_task = 0;  // 周期扫描任务句柄（0=未注册）；仅持 g_task_mtx 访问
std::atomic<bool> g_global_enabled{false};  // 全局开关；JVM 线程写、主线程读
std::atomic<bool> g_save_active{false};     // 已进入存档（save-enter 置位）；主线程写

struct ScanStats {
    int sold = 0;
    int failed = 0;
};

struct PhysicalScanCtx {
    const autosell::Config* cfg;
    ScanStats* stats;
};

// 安全门控：世界态 + g_gamestate==0 + 扩展背包模块视图未安装。
bool scan_gates_ok() {
    if (!game_in_world()) return false;
    const uint32_t gamestate =
        g_gamestate != nullptr ? *reinterpret_cast<uint32_t*>(g_gamestate) : 0;
    if (gamestate != 0) return false;
    if (extension_bag_module_view_installed()) return false;
    return true;
}

void process_ref(const InventoryItemRef& ref, const autosell::Config& cfg, ScanStats* stats) {
    autosell::ItemView view;
    if (!autosell_build_view(ref, &view)) return;
    if (!autosell::should_sell(view, cfg)) return;

    // I-2 NoSell 预过滤：不可售物品直接跳过，不计失败、不打 WARN
    // （否则 inventory_trade::sell 会以 WARN 记 no_sell 并计入失败）。
    if (fn_item_is_no_sell != nullptr && fn_item_is_no_sell(ref.category) != 0) {
        return;
    }

    // I-1：把建视图用的物品指针传入，处置前做身份复核（并发槽位改动则不结算）。
    const inventory_trade::Result result =
        inventory_trade::sell(ref.bag, ref.slot, ref.native_item, "autosell");
    if (result.status == inventory_trade::Status::kOk) {
        ++stats->sold;
        return;
    }
    // 槽位已变/物品已不在（kStale/kEmptySlot/kNoNativeItem）：跳过，不计失败。
    if (result.status == inventory_trade::Status::kStale ||
        result.status == inventory_trade::Status::kEmptySlot ||
        result.status == inventory_trade::Status::kNoNativeItem) {
        return;
    }
    ++stats->failed;
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "sell failed bag=%d slot=%d category=%d status=%d",
                        ref.bag, ref.slot, ref.category, static_cast<int>(result.status));
}

void scan_body(const autosell::Config& cfg, int64_t frame) {
    ScanStats stats;

    // 原版物理袋 0..4（for_each_bag_slot 含任务袋 5，回调内排除）。
    PhysicalScanCtx physical{&cfg, &stats};
    for_each_bag_slot([](void* item, int bag, int slot, void* ctx) -> bool {
        if (bag > kLastPhysicalBag) return false;
        if (item == nullptr) return false;
        PhysicalScanCtx* c = static_cast<PhysicalScanCtx*>(ctx);
        const uint16_t flags =
            *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(item) + I_TYPE);
        const int category = fn_get_bit != nullptr ? fn_get_bit(flags, 15, 6) : 0;
        if (category <= 0) return false;
        InventoryItemRef ref;
        ref.kind = InventoryItemKind::kOriginal;
        ref.bag = bag;
        ref.slot = slot;
        ref.category = category;
        ref.count = fn_get_cumulate_count != nullptr ? fn_get_cumulate_count(item) : 1;
        ref.native_item = item;
        process_ref(ref, *c->cfg, c->stats);
        return false;  // 继续遍历
    }, &physical);

    // 扩展逻辑袋 6..10：逐槽经稳定端口读取（每次自行取放锁）。
    for (int bag = kLogicalBagScanFirst; bag < kLogicalBagScanFirst + kLogicalBagScanCount; ++bag) {
        if (!extension_bag_is_logical_bag(bag)) continue;
        for (int slot = 0; slot < kSlotCount; ++slot) {
            InventoryItemRef ref;
            if (!inventory_item_ref_at(bag, slot, &ref)) continue;
            if (ref.kind != InventoryItemKind::kExtension) continue;
            if (ref.native_item == nullptr || ref.category <= 0) continue;
            process_ref(ref, cfg, &stats);
        }
    }

    autosell_note_scan(frame, stats.sold, stats.failed);
    if (stats.sold > 0 || stats.failed > 0) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "scan frame=%lld sold=%d failed=%d",
                            static_cast<long long>(frame), stats.sold, stats.failed);
    }
}

// frame_task 回调：每次调用现取运行时配置，无跨帧状态；60 帧节流由 frame_task 的
// interval 负责。返回 false = 任务完成（自动注销）；本任务无限期运行，恒返回 true。
bool autosell_tick(int64_t frame, void* /*ctx*/) {
    // 全局开关防御：关闭态不应有任务存在；即使任务删除存在竞态也直接跳过。
    if (!g_global_enabled.load(std::memory_order_acquire)) return true;
    const autosell::Config cfg = autosell_get_runtime_config();
    if (!cfg.enabled) return true;   // 防御：关闭态不应有任务存在
    if (!scan_gates_ok()) return true;

    std::lock_guard<std::mutex> lock(g_scan_mtx);
    scan_body(cfg, frame);
    return true;
}

// 无条件删除已注册任务（兜底）：不读取全局/存档配置状态，已有任务即删除。
void remove_task_if_any() {
    std::lock_guard<std::mutex> lock(g_task_mtx);
    if (g_task == 0) return;
    frame_task_remove(g_task);
    g_task = 0;
    __android_log_print(ANDROID_LOG_INFO, kTag, "task removed");
}

// 按「全局开关已武装 && 已进入存档 && 存档配置总开关开启」同步 60 帧周期任务；幂等。
// 满足则注册；不满足一律走 remove_task_if_any 兜底删除（不依赖配置状态）。
void sync_task() {
    const bool want = g_global_enabled.load(std::memory_order_acquire) &&
                      g_save_active.load(std::memory_order_acquire) &&
                      autosell_get_runtime_config().enabled;
    if (!want) {
        remove_task_if_any();
        return;
    }
    std::lock_guard<std::mutex> lock(g_task_mtx);
    if (g_task != 0) return;
    g_task = frame_task_add(kFramePointRenderPre, &autosell_tick, nullptr,
                            kAutoSellScanIntervalFrames, 0);
    __android_log_print(ANDROID_LOG_INFO, kTag, "task registered id=%llu",
                        static_cast<unsigned long long>(g_task));
}

// save_enter 回调（游戏主线程）：按当前存档槽加载 sidecar 配置应用到运行时，再同步任务。
void autosell_on_save_enter(void* /*ctx*/) {
    const int slot = current_save_slot();
    autosell_store_ensure_loaded(slot);
    g_save_active.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag, "save enter slot=%d; sync task", slot);
    sync_task();
}

// save_exit 回调（游戏主线程）：退出存档（world -> 主菜单）时置「未进档」并无条件删除任务
// （兜底：不依赖存档配置的开启状态）。
void autosell_on_save_exit(void* /*ctx*/) {
    g_save_active.store(false, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag, "save exit; remove task");
    remove_task_if_any();
}

}  // namespace

void autosell_apply_config(const autosell::Config& config) {
    // UI 关闭/销毁时提交一次（面板内编辑不实时持久化）：写入运行时配置后，**仅当存档总开关
    // enabled 发生切换**才同步任务；规则/阈值等改动不触碰任务生命周期（减少注册/删除时机）。
    const bool old_enabled = autosell_get_runtime_config().enabled;
    autosell_set_runtime_config(config);
    if (old_enabled == config.enabled) return;
    __android_log_print(ANDROID_LOG_INFO, kTag, "save master switch %d -> %d",
                        old_enabled ? 1 : 0, config.enabled ? 1 : 0);
    if (config.enabled) {
        sync_task();                 // 开：按条件注册
    } else {
        remove_task_if_any();        // 关：无条件删除（兜底，不依赖配置状态）
    }
}

void autosell_set_global_enabled(bool enabled) {
    g_global_enabled.store(enabled, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag, "global enabled=%d save_active=%d",
                        enabled ? 1 : 0,
                        g_save_active.load(std::memory_order_acquire) ? 1 : 0);
    if (enabled) {
        sync_task();                 // 开：按条件注册
    } else {
        remove_task_if_any();        // 关：无条件删除（兜底，不依赖配置状态）
    }
}

void autosell_register_save_enter() {
    // 幂等：同一 fn+ctx 重复注册不会叠加。
    save_enter_register(&autosell_on_save_enter, nullptr);
}

void autosell_register_save_exit() {
    // 幂等：world -> 主菜单时若已有任务即删除。
    save_exit_register(&autosell_on_save_exit, nullptr);
}
