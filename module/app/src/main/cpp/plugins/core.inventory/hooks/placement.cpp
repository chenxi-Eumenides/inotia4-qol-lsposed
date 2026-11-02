#include "placement.h"

#include <cstdint>

#include "contracts/game_ports/item_port.h"
#include "contracts/inventory_identity.h"
#include "contracts/result.h"
#include "plugins/core.inventory/api/bag_source.h"
#include "plugins/core.inventory/runtime/context.h"

namespace qol::features::core_inventory::hooks::placement {
namespace {

using qol::contracts::QolBagInfo;
using qol::contracts::QolBagRef;
using qol::contracts::QolInventoryItemRef;

// count_limit 不可用时的合并预检上限；预检通过但真实写入超限的失败由调用方处理。
// 不使用原版 126/99 硬编码：999 模式下合并写入走全量 a+b，超限只在写入侧失败。
constexpr uint32_t kPrecheckStackLimit = 999;

bool native_read_ready() {
    return g_port != nullptr && qol::contracts::qol_item_port_bag_space_size_valid(g_port) &&
           g_port->read.bag_count != nullptr && g_port->read.bag_info != nullptr &&
           g_port->read.item_at != nullptr;
}

bool serialize_ready() {
    return g_port != nullptr && qol::contracts::qol_item_port_transfer_size_valid(g_port) &&
           g_port->read.serialize_item != nullptr;
}

// 合并预检上限：优先目标堆自身的 count_limit（原生 item port 逐堆提供视图上限）；
// 注册源句柄不在原生句柄表内，count_limit 失败时统一退回 999 预检。
uint32_t merge_stack_limit(const QolInventoryItemRef& existing) {
    if (g_port != nullptr && qol::contracts::qol_item_port_size_valid(g_port) &&
        g_port->read.count_limit != nullptr) {
        uint32_t limit = 0;
        if (qol::contracts::succeeded(g_port->read.count_limit(existing.item, &limit)) &&
            limit > 0) {
            return limit;
        }
    }
    return kPrecheckStackLimit;
}

bool merge_count_fits(const QolInventoryItemRef& existing,
                      const contracts::QolBagTransfer& transfer) {
    const int64_t total = static_cast<int64_t>(existing.count) + transfer.count;
    return total > 0 && total <= static_cast<int64_t>(merge_stack_limit(existing));
}

bool scope_bag_selected(int32_t candidate_bag, int32_t scope_bag) {
    return scope_bag < 0 || candidate_bag == scope_bag;
}

// ---------------------------------------------------------------------------
// 原生空间（space0 原版 / space1 C18）：g_port->read 袋扫描。
// 任务袋（space0 bag5）在 merge 与 empty 两条路径都跳过，永不返回。
// ---------------------------------------------------------------------------

bool scan_native_merge(int32_t space_id, const contracts::QolBagTransfer& transfer,
                       int32_t scope_bag, PlacementTarget* out) {
    if (!native_read_ready() || !serialize_ready()) return false;
    int32_t bag_count = 0;
    if (!qol::contracts::succeeded(g_port->read.bag_count(space_id, &bag_count)) ||
        bag_count < 0) {
        return false;
    }
    for (int32_t bag = 0; bag < bag_count; ++bag) {
        if (space_id == qol::contracts::kQolBagSpaceOriginal &&
            bag == qol::contracts::kQolTaskBagIndex) {
            continue;
        }
        if (!scope_bag_selected(bag, scope_bag)) continue;
        const QolBagRef ref{space_id, bag};
        QolBagInfo info{};
        if (!qol::contracts::succeeded(g_port->read.bag_info(&ref, &info)) ||
            info.capacity < 0) {
            continue;
        }
        for (int32_t slot = 0; slot < info.capacity; ++slot) {
            QolInventoryItemRef existing{};
            const QolResult probe = g_port->read.item_at(&ref, slot, &existing);
            if (probe.code == kQolErrNotFound) continue;
            if (!qol::contracts::succeeded(probe)) continue;
            // category 预过滤先行，避免对每个候选做完整序列化。
            if (existing.category != transfer.category || existing.count <= 0) continue;
            if (!merge_count_fits(existing, transfer)) continue;
            uint8_t payload[contracts::kQolBagTransferPayloadCapacity]{};
            uint32_t size = 0;
            if (!qol::contracts::succeeded(g_port->read.serialize_item(
                    existing.item, payload, sizeof(payload), &size))) {
                continue;
            }
            if (!qol::contracts::qol_inventory_payload_identity_equal(
                    payload, size, transfer.payload, transfer.payload_size)) {
                continue;
            }
            *out = PlacementTarget{space_id, bag, slot, true};
            return true;
        }
    }
    return false;
}

bool scan_native_empty(int32_t space_id, int32_t scope_bag, PlacementTarget* out) {
    if (!native_read_ready()) return false;
    int32_t bag_count = 0;
    if (!qol::contracts::succeeded(g_port->read.bag_count(space_id, &bag_count)) ||
        bag_count < 0) {
        return false;
    }
    for (int32_t bag = 0; bag < bag_count; ++bag) {
        if (space_id == qol::contracts::kQolBagSpaceOriginal &&
            bag == qol::contracts::kQolTaskBagIndex) {
            continue;
        }
        if (!scope_bag_selected(bag, scope_bag)) continue;
        const QolBagRef ref{space_id, bag};
        QolBagInfo info{};
        if (!qol::contracts::succeeded(g_port->read.bag_info(&ref, &info)) ||
            info.capacity < 0) {
            continue;
        }
        for (int32_t slot = 0; slot < info.capacity; ++slot) {
            QolInventoryItemRef existing{};
            const QolResult probe = g_port->read.item_at(&ref, slot, &existing);
            if (probe.code == kQolErrNotFound) {
                *out = PlacementTarget{space_id, bag, slot, false};
                return true;
            }
            if (!qol::contracts::succeeded(probe)) continue;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 注册源空间（space>=2）：bag source 读面 + export_item 取身份。
// ---------------------------------------------------------------------------

bool scan_registered_merge(const qol::contracts::QolBagSource* source,
                           const contracts::QolBagTransfer& transfer, int32_t scope_bag,
                           PlacementTarget* out) {
    if (source == nullptr || source->count == nullptr || source->info == nullptr ||
        source->item_at == nullptr || source->export_item == nullptr) {
        return false;
    }
    const int32_t bag_count = source->count();
    if (bag_count < 0) return false;
    for (int32_t bag = 0; bag < bag_count; ++bag) {
        if (!scope_bag_selected(bag, scope_bag)) continue;
        QolBagInfo info{};
        if (!qol::contracts::succeeded(source->info(bag, &info)) || info.capacity < 0) {
            continue;
        }
        for (int32_t slot = 0; slot < info.capacity; ++slot) {
            QolInventoryItemRef existing{};
            const QolResult probe = source->item_at(bag, slot, &existing);
            if (probe.code == kQolErrNotFound) continue;
            if (!qol::contracts::succeeded(probe)) continue;
            if (existing.category != transfer.category || existing.count <= 0) continue;
            if (!merge_count_fits(existing, transfer)) continue;
            contracts::QolBagTransfer payload{};
            if (!qol::contracts::succeeded(source->export_item(existing.item, 1, &payload))) {
                continue;
            }
            if (!qol::contracts::qol_inventory_payload_identity_equal(
                    payload.payload, payload.payload_size, transfer.payload,
                    transfer.payload_size)) {
                continue;
            }
            *out = PlacementTarget{source->space_id, bag, slot, true};
            return true;
        }
    }
    return false;
}

bool scan_registered_empty(const qol::contracts::QolBagSource* source, int32_t scope_bag,
                           PlacementTarget* out) {
    if (source == nullptr || source->count == nullptr || source->info == nullptr ||
        source->item_at == nullptr) {
        return false;
    }
    const int32_t bag_count = source->count();
    if (bag_count < 0) return false;
    for (int32_t bag = 0; bag < bag_count; ++bag) {
        if (!scope_bag_selected(bag, scope_bag)) continue;
        QolBagInfo info{};
        if (!qol::contracts::succeeded(source->info(bag, &info)) || info.capacity < 0) {
            continue;
        }
        for (int32_t slot = 0; slot < info.capacity; ++slot) {
            QolInventoryItemRef existing{};
            const QolResult probe = source->item_at(bag, slot, &existing);
            if (probe.code == kQolErrNotFound) {
                *out = PlacementTarget{source->space_id, bag, slot, false};
                return true;
            }
            if (!qol::contracts::succeeded(probe)) continue;
        }
    }
    return false;
}

bool scan_registered_sources(const contracts::QolBagTransfer& transfer, bool merge_pass,
                             int32_t scope_bag, PlacementTarget* out) {
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const auto* source = registered_bag_source_at(index);
        if (source == nullptr ||
            source->space_id < qol::contracts::kQolBagSpaceFirstRegistered) {
            continue;
        }
        const bool hit = merge_pass ? scan_registered_merge(source, transfer, scope_bag, out)
                                    : scan_registered_empty(source, scope_bag, out);
        if (hit) return true;
    }
    return false;
}

}  // namespace

bool resolve_placement(const contracts::QolBagTransfer& transfer, bool stackable,
                       int32_t scope_space, int32_t scope_bag, PlacementTarget* out) {
    if (out == nullptr || transfer.category <= 0 || transfer.count <= 0 ||
        transfer.payload_size == 0 ||
        transfer.payload_size > qol::contracts::kQolBagTransferPayloadMaxSize) {
        return false;
    }
    if (scope_bag >= 0 && scope_space < 0) return false;

    if (scope_space < 0) {
        // 全局严格六步：原生合并 → 注册源合并（注册序）→ C18 合并 →
        // 原生空槽 → 注册源空槽（注册序）→ C18 空槽。
        if (stackable &&
            scan_native_merge(qol::contracts::kQolBagSpaceOriginal, transfer, -1, out)) {
            return true;
        }
        if (stackable && scan_registered_sources(transfer, true, -1, out)) return true;
        if (stackable &&
            scan_native_merge(qol::contracts::kQolBagSpaceVariant, transfer, -1, out)) {
            return true;
        }
        if (scan_native_empty(qol::contracts::kQolBagSpaceOriginal, -1, out)) return true;
        if (scan_registered_sources(transfer, false, -1, out)) return true;
        if (scan_native_empty(qol::contracts::kQolBagSpaceVariant, -1, out)) return true;
        return false;
    }

    if (scope_space == qol::contracts::kQolBagSpaceOriginal ||
        scope_space == qol::contracts::kQolBagSpaceVariant) {
        if (stackable && scan_native_merge(scope_space, transfer, scope_bag, out)) return true;
        return scan_native_empty(scope_space, scope_bag, out);
    }
    if (scope_space >= qol::contracts::kQolBagSpaceFirstRegistered) {
        const auto* source = find_bag_source(scope_space);
        if (source == nullptr) return false;
        if (stackable && scan_registered_merge(source, transfer, scope_bag, out)) return true;
        return scan_registered_empty(source, scope_bag, out);
    }
    return false;
}

}  // namespace qol::features::core_inventory::hooks::placement
