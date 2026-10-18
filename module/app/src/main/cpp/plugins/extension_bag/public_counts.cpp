#include "game_ui_virtbag.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/extension_bag_equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/extension_bag_logging.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/store_sell_rules.h"
#include "plugins/extension_bag/extension_bag_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

uint32_t virtual_bag_effective_count(const void* item, uint32_t fallback_canonical) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (item != nullptr) {
        if (api != nullptr && api->effective != nullptr) {
            uint32_t count = 0;
            const QolResult result = api->effective(item, &count);
            if (qol::contracts::succeeded(result)) {
                // 关闭态 effective 视图可能为 0（canonical&0x7F==0），而 descriptor canonical
                // 已证明该槽有物；回 0 会被下游当非法 count 丢弃。此时取 canonical 视图
                // （R-47 决策 b：启用态 canonical、关闭态 b）。真值：canonical=0→0；
                // effective>0→effective；effective==0 且 canonical>0→canonical 视图。
                if (count != 0 || fallback_canonical == 0) return count;
                return virtual_bag_view_from_canonical(fallback_canonical, /*extended=*/true);
            }
            // 任何失败（含 unsupported，如 ITEMCLASS 表不可解）都不回 0：回 0 会被下游
            // 当作非法 count 而使整段 EB 快照失效。改为回退原版视图（G-item-#10 口径：
            // 能力缺失或不可用时回退原版 getter）。
        }
        uint32_t field = 0;
        if (api != nullptr && api->read_encoded_field != nullptr &&
            qol::contracts::succeeded(api->read_encoded_field(item, &field))) {
            const uint32_t view = original_count_view(field);
            // 活体对象缺失/不可解时 field 可读但 b 段为 0（如 canonical 的低 7 位为 0）。
            // descriptor canonical 已证明该槽有物，回 0 会被下游当非法 count 丢弃。
            if (view != 0) return view;
        }
    }
    // 启用态输出 canonical 视图（R-47 决策 b：启用态 canonical、关闭态 b）；本函数仅在
    // EB 启用时经 for_each_logical_item 调用，故兜底取 canonical 视图而非 b 视图，
    // 避免 canonical&0x7F==0 的投影槽被当作非法 count 跳过。
    return virtual_bag_view_from_canonical(fallback_canonical, /*extended=*/true);
}

uint32_t virtual_bag_canonical_from_field(uint32_t field, uint32_t fallback_canonical) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->canonical_from_field != nullptr) {
        uint32_t canonical = fallback_canonical;
        const QolResult result = api->canonical_from_field(field, &canonical);
        if (qol::contracts::succeeded(result)) return canonical;
    }
    return fallback_canonical;
}

uint32_t virtual_bag_field_from_canonical(uint32_t field, uint32_t canonical, bool extended) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->field_from_canonical != nullptr) {
        uint32_t updated = field;
        const QolResult result = api->field_from_canonical(
            field, canonical, extended ? 1 : 0, &updated);
        if (qol::contracts::succeeded(result)) return updated;
    }
    return field;
}

uint32_t virtual_bag_view_from_canonical(uint32_t canonical, bool extended) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->view_from_canonical != nullptr) {
        uint32_t view = canonical;
        const QolResult result = api->view_from_canonical(canonical, extended ? 1 : 0, &view);
        if (qol::contracts::succeeded(result)) return view;
    }
    return canonical;
}

uint32_t virtual_bag_count_limit(bool extended) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->view_limit != nullptr) {
        uint32_t limit = 0;
        const QolResult result = api->view_limit(extended ? 1 : 0, &limit);
        if (qol::contracts::succeeded(result)) return limit;
    }
    return 0;
}

uint32_t virtual_bag_canonical_count(const void* item, uint32_t fallback_canonical) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api != nullptr && api->canonical != nullptr && item != nullptr) {
        uint32_t count = 0;
        const QolResult result = api->canonical(item, &count);
        if (qol::contracts::succeeded(result)) return count;
        if (result.code == kQolErrUnsupported) {
            return fallback_canonical;
        }
    }
    if (item != nullptr) {
        uint32_t field = 0;
        if (api == nullptr || api->read_encoded_field == nullptr ||
            !qol::contracts::succeeded(api->read_encoded_field(item, &field))) {
            return fallback_canonical;
        }
        return virtual_bag_canonical_from_field(field, fallback_canonical);
    }
    return fallback_canonical;
}

bool virtual_bag_extended_mode(const void* item) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (item == nullptr || api == nullptr || api->read_extended_mode == nullptr) return false;
    // OK1 表示 999 请求运行且整组资源已实际应用；NotReady（请求未落实）与 OK0（明确关闭）
    // 都不进入扩展视图，避免把未落地的模式当作关闭而回退原版语义。
    uint8_t extended = 0;
    const QolResult mode_result = api->read_extended_mode(&extended);
    if (!qol::contracts::succeeded(mode_result) || extended == 0) return false;
    if (api->effective == nullptr) return false;
    uint32_t effective = 0;
    const QolResult effective_result = api->effective(item, &effective);
    return qol::contracts::succeeded(effective_result);
}

bool virtual_bag_write_effective(void* item, uint32_t count) {
    const qol::contracts::QolItemCountApi* api = item_count_api();
    if (api == nullptr || api->write_effective == nullptr ||
        api->writeback_needed == nullptr || item == nullptr) {
        return false;
    }
    int32_t needed = 0;
    const QolResult needed_result = api->writeback_needed(item, count, &needed);
    // NotReady 表示 999 请求已开但资源组/适配尚未落地。原版写法对 canonical 的安全性未
    // 证明，故不回退 original_count_write；直接使该写请求失败并保留原错误语义。
    if (needed_result.code == kQolErrNotReady) return false;
    if (!qol::contracts::succeeded(needed_result)) return false;
    if (needed == 0) return true;
    const QolResult write_result = api->write_effective(item, count);
    if (write_result.code == kQolErrNotReady) return false;
    return qol::contracts::succeeded(write_result);
}
// 与 item_count_encoding/ITEM_GetCumulateCount 使用同一 ITEMCLASSBASE +6 bit0 语义：
// kUnknown 必须继续向载入/合并调用方传播，不能降级成可堆叠。
::qol::contracts::QolItemCountEncoding virtual_bag_category_uses_stack_count(int category) {
    return category_is_equip(category);
}
