// autosell_scan.cpp —— 自动出售扫描与主线程逐帧 tick（阶段 B）。
//
// 扫描范围：原版物理袋 0..4（for_each_bag_slot，排除任务袋 5）+ 扩展逻辑袋 5 个
// （6..10，经稳定端口 inventory_item_ref_at 逐槽读取，每次调用自行取放锁；
// 不在 extension_bag_for_each_logical_item 回调内处置）。
// 处置：autosell_build_view -> should_sell -> NoSell 预过滤
//       -> inventory_trade::sell(bag, slot, ref.native_item, "autosell")（I-1 身份复核）。

#include "feature/autosell/autosell_scan.h"

#include "core/native/extension_bag_port.h"
#include "core/native/frame_tick.h"
#include "core/native/inventory_trade.h"
#include "data/native/game_symbols.h"
#include "feature/autosell/autosell_config.h"
#include "feature/autosell/autosell_store.h"
#include "feature/autosell/autosell_view.h"
#include "game_access.h"
#include "game_state.h"

#include <android/log.h>

#include <cstdint>
#include <mutex>

namespace {

constexpr char kTag[] = "Inotia4AutoSell";
constexpr int kLastPhysicalBag = 4;      // 原版任务袋 5 排除
constexpr int kLogicalBagScanFirst = 6;  // = kExtensionLogicalBagFirst
constexpr int kLogicalBagScanCount = 5;  // = kBagCount
constexpr int kSlotCount = 16;

std::mutex g_scan_mtx;
int64_t g_last_scan_frame = -1;  // 仅主线程访问；<0 表示从未扫描

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

void scan_body(const autosell::Config& cfg) {
    ScanStats stats;
    const int64_t frame = frame_tick_current_frame();

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
        __android_log_print(ANDROID_LOG_INFO, kTag, "scan frame=%lld sold=%d failed=%d",
                            static_cast<long long>(frame), stats.sold, stats.failed);
    }
}

}  // namespace

void autosell_init() {
    frame_tick_register(&autosell_tick, nullptr);
}

void autosell_tick(void* /*ctx*/) {
    // 世界态门控后、节流前：按当前存档槽加载 sidecar 配置（slot 未变时零 IO）。
    if (!scan_gates_ok()) return;
    autosell_store_ensure_loaded(current_save_slot());

    const autosell::Config cfg = autosell_get_runtime_config();
    if (!cfg.enabled) return;

    const int64_t frame = frame_tick_current_frame();
    const bool immediate = autosell_consume_immediate_run();
    if (!autosell_should_scan(frame, g_last_scan_frame, immediate)) {
        return;
    }
    g_last_scan_frame = frame;

    std::lock_guard<std::mutex> lock(g_scan_mtx);
    scan_body(cfg);
}

void autosell_scan_once() {
    const autosell::Config cfg = autosell_get_runtime_config();
    if (!cfg.enabled) return;
    if (!scan_gates_ok()) return;
    std::lock_guard<std::mutex> lock(g_scan_mtx);
    scan_body(cfg);
}
