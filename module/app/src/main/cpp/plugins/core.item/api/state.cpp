#include "plugins/core.item/runtime/core_item_internal.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "contracts/extension_bag.h"
#include "plugins/core.item/http/response.h"

namespace qol::features::core_item {

static bool g_snapshot_serialize_warned = false;

void log_snapshot_serialize_failure() {
    if (g_snapshot_serialize_warned) return;
    g_snapshot_serialize_warned = true;
    constexpr char kMessage[] = "[core.item] snapshot serialize failed";
    if (g_sdk == nullptr || g_sdk->log.write == nullptr) return;
    (void)g_sdk->log.write(2, 1, kPluginId, kMessage,
                           static_cast<uint32_t>(sizeof(kMessage) - 1));
}

uint32_t g_degrade_warn_ticks = 0;

void log_degrade_warn(const char* message) {
    if (message == nullptr) return;
    if (++g_degrade_warn_ticks % 300u != 1u) return;
    if (g_sdk == nullptr || g_sdk->log.write == nullptr) return;
    uint32_t length = 0;
    while (message[length] != '\0') ++length;
    (void)g_sdk->log.write(2, 1, kPluginId, message, length);
}

void publish_not_in_game(ItemSnapshot& snapshot) {
    snapshot.state = SnapshotState::kNotInGame;
    (void)copy_buffer(snapshot.inventory, kInventoryCapacity, &snapshot.inventory_size,
                      kNotInGameBody, static_cast<uint32_t>(sizeof(kNotInGameBody) - 1));
    (void)copy_buffer(snapshot.shop, kShopCapacity, &snapshot.shop_size, kNotInGameBody,
                      static_cast<uint32_t>(sizeof(kNotInGameBody) - 1));
    g_snapshot.publish(snapshot);
}

QolResult append_extension_inventory_bags(std::string* out) {
    if (out == nullptr || g_port == nullptr) return contracts::ok();
    const InventoryExtensionAccessResult access = inventory_extension();
    if (!access.available || access.provider == nullptr ||
        access.provider->inventory_json_fragment == nullptr) {
        // provider 未注册/停用 → 省略扩展段（限速 WARN 保留可诊断性）；主快照照常。
        log_degrade_warn("extension inventory omitted: provider unavailable");
        return contracts::ok();
    }
    int32_t in_world = 0;
    if (g_port->read.in_world == nullptr ||
        !contracts::succeeded(g_port->read.in_world(&in_world)) || in_world == 0) {
        return contracts::ok();
    }
    thread_local std::vector<char> fragment(kInventoryCapacity);
    uint32_t fragment_size = 0;
    const QolResult result = access.provider->inventory_json_fragment(
        fragment.data(), static_cast<uint32_t>(fragment.size()), &fragment_size);
    if (!contracts::succeeded(result) || fragment_size == 0) {
        // provider 门禁关闭或不可用时贡献空片段：省略扩展段，主快照/写票据照常。
        log_degrade_warn("extension inventory omitted: fragment unavailable");
        return contracts::ok();
    }
    out->append(fragment.data(), fragment_size);
    return contracts::ok();
}

QolResult serialize_inventory_json(char* out, uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr || g_port == nullptr ||
        g_port->read.in_world == nullptr || g_port->read.item_ref_at == nullptr ||
        g_port->read.inventory_item_view == nullptr) {
        return error_result(kQolErrInvalidArgument, "inventory serializer unavailable");
    }
    *out_size = 0;
    int32_t in_world = 0;
    if (!contracts::succeeded(g_port->read.in_world(&in_world))) return not_ready();
    if (in_world == 0) {
        constexpr char kNotInWorld[] = "{\"error\":\"not in game\"}";
        if (sizeof(kNotInWorld) - 1 > capacity) {
            return error_result(kQolErrInvalidArgument, "inventory buffer too small");
        }
        std::memcpy(out, kNotInWorld, sizeof(kNotInWorld) - 1);
        *out_size = sizeof(kNotInWorld) - 1;
        return contracts::ok();
    }

    std::string json = "{\"bags\":[";
    for (int32_t bag = 0; bag < 6; ++bag) {
        if (bag != 0) json.push_back(',');
        json += "{\"bag\":" + std::to_string(bag) + ",\"items\":[";
        int32_t filled = 0;
        for (int32_t slot = 0; slot < 16; ++slot) {
            contracts::QolInventoryItemRef item_ref{};
            const QolResult ref_result = g_port->read.item_ref_at(bag, slot, &item_ref);
            if (!contracts::succeeded(ref_result)) {
                if (ref_result.code == kQolErrNotFound) continue;
                return ref_result;
            }
            if (item_ref.kind != static_cast<int32_t>(contracts::QolInventoryItemKind::kOriginal) ||
                item_ref.item.slot == 0) {
                continue;
            }
            contracts::QolItemInventoryView view{};
            const QolResult view_result = g_port->read.inventory_item_view(item_ref.item,
                                                                            &view);
            if (!contracts::succeeded(view_result)) return view_result;
            if (!inventory_option_counts_valid(view)) {
                return error_result(kQolErrInvalidArgument, "invalid item option count");
            }
            uint32_t count = 0;
            bool count_present = false;
            if (!effective_item_count(item_ref.item, view.count_supported != 0, &count,
                                      &count_present)) {
                return error_result(kQolErrNotReady, "item count unavailable");
            }
            if (filled != 0) json.push_back(',');
            append_inventory_item(&json, slot, view, count_present, count);
            ++filled;
        }
        json += "],\"capacity\":16,\"slot_count\":" + std::to_string(filled) + "}";
    }
    const QolResult extension_result = append_extension_inventory_bags(&json);
    if (!contracts::succeeded(extension_result)) return extension_result;
    json += "]}";
    if (json.size() > capacity) {
        return error_result(kQolErrInvalidArgument, "inventory buffer too small");
    }
    std::memcpy(out, json.data(), json.size());
    *out_size = static_cast<uint32_t>(json.size());
    return contracts::ok();
}

QolResult write_item_state_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (out == nullptr || out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid item state output");
    }
    switch (static_cast<contracts::QolItemStateKind>(kind)) {
        case contracts::QolItemStateKind::kRawInventory:
            return serialize_inventory_json(out, capacity, out_size);
        case contracts::QolItemStateKind::kEnrichedInventory: {
            const auto write_error = [out, capacity, out_size](const char* error) {
                const std::string body = std::string("{\"ok\":false,\"error\":\"") + error +
                                         "\"}";
                if (body.size() > capacity) {
                    return error_result(kQolErrBlocked, "item state buffer too small");
                }
                std::memcpy(out, body.data(), body.size());
                *out_size = static_cast<uint32_t>(body.size());
                return contracts::ok();
            };
            static thread_local std::vector<char> raw_inventory(kInventoryCapacity);
            uint32_t raw_size = 0;
            const QolResult serialized = serialize_inventory_json(
                raw_inventory.data(), static_cast<uint32_t>(raw_inventory.size()), &raw_size);
            if (!contracts::succeeded(serialized)) return serialized;
            if (std::strncmp(raw_inventory.data(), "{\"error\":", 9) == 0) {
                if (raw_size > capacity) {
                    return error_result(kQolErrBlocked, "item state buffer too small");
                }
                std::memcpy(out, raw_inventory.data(), raw_size);
                *out_size = raw_size;
                return contracts::ok();
            }
            std::string body;
            if (!enrich_inventory(raw_inventory.data(), raw_size, g_port, &body)) {
                return write_error("internal error");
            }
            if (body.size() > capacity) {
                return error_result(kQolErrBlocked, "item state buffer too small");
            }
            std::memcpy(out, body.data(), body.size());
            *out_size = static_cast<uint32_t>(body.size());
            return contracts::ok();
        }
        default:
            return error_result(kQolErrInvalidArgument, "invalid item state kind");
    }
}

void update_snapshot() {
    if (g_port == nullptr || g_port->read.inventory_item_view == nullptr ||
        g_port->read.item_ref_at == nullptr ||
        g_port->read.shop_items_json == nullptr || g_port->read.money == nullptr ||
        g_port->read.in_world == nullptr) {
        log_snapshot_serialize_failure();
        return;
    }
    // 静态名称表未下发时不发布，避免输出空名称；HTTP 侧表现为 not ready。
    if (g_port->read.static_ready != nullptr) {
        int32_t ready = 0;
        if (!contracts::succeeded(g_port->read.static_ready(&ready))) {
            log_snapshot_serialize_failure();
            return;
        }
        if (ready == 0) return;
    }

    ItemSnapshot next{};
    int32_t in_world = 0;
    if (!contracts::succeeded(g_port->read.in_world(&in_world))) {
        log_snapshot_serialize_failure();
        return;
    }
    if (in_world == 0) {
        publish_not_in_game(next);
        return;
    }

    thread_local std::vector<char> inventory(kInventoryCapacity);
    std::fill(inventory.begin(), inventory.end(), '\0');
    uint32_t inventory_size = 0;
    if (!contracts::succeeded(serialize_inventory_json(inventory.data(), kInventoryCapacity,
                                                       &inventory_size))) {
        log_snapshot_serialize_failure();
        return;
    }
    if (std::strncmp(inventory.data(), "{\"error\":", 9) == 0) {
        publish_not_in_game(next);
        return;
    }
    if (!copy_buffer(next.inventory, kInventoryCapacity, &next.inventory_size, inventory.data(),
                     inventory_size)) {
        log_snapshot_serialize_failure();
        return;
    }

    char shop[kShopCapacity]{};
    uint32_t shop_size = 0;
    if (!contracts::succeeded(g_port->read.shop_items_json(shop, kShopCapacity, &shop_size))) {
        log_snapshot_serialize_failure();
        return;
    }
    if (!copy_buffer(next.shop, kShopCapacity, &next.shop_size, shop, shop_size)) {
        log_snapshot_serialize_failure();
        return;
    }

    if (!contracts::succeeded(g_port->read.money(&next.money))) {
        log_snapshot_serialize_failure();
        return;
    }
    next.state = SnapshotState::kReady;
    g_snapshot.publish(next);
}

int32_t frame_tick(int64_t, void*) {
    update_snapshot();
    return 1;
}
extern const contracts::ItemStateApi kItemStateApi{&write_item_state_json};

}  // namespace qol::features::core_item
