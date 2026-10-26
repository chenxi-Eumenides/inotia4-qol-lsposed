#include "plugins/core.item/runtime/core_item_internal.h"

#include <cstring>
#include <limits>
#include <string>

#include "plugins/core.item/bags/snapshot_store.h"
#include "plugins/core.item/http/response.h"
#include "plugins/core.item/shop/snapshot.h"
#ifndef QOL_CORE_ITEM_SHOP_SEPARATE
#include "plugins/core.item/shop/snapshot.cpp"
#endif
#ifndef QOL_CORE_ITEM_TYPED_READ_SOURCES_SEPARATELY
#include "plugins/core.item/bags/snapshot_store.cpp"
#include "plugins/core.item/usecases/read_inventory.cpp"
#endif

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
    bags::inventory_snapshot_store().clear();
    snapshot.state = SnapshotState::kNotInGame;
    (void)copy_buffer(snapshot.inventory, kInventoryCapacity, &snapshot.inventory_size,
                      kNotInGameBody, static_cast<uint32_t>(sizeof(kNotInGameBody) - 1));
    g_snapshot.publish(snapshot);
}

QolResult append_registered_inventory_bags(std::string* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid inventory output");
    for (uint32_t source_index = 0; source_index < registered_bag_source_count(); ++source_index) {
        const contracts::QolBagSource* source = registered_bag_source_at(source_index);
        if (source == nullptr || source->count == nullptr || source->info == nullptr ||
            source->item_at == nullptr || source->space_id < contracts::kQolBagSpaceFirstRegistered) {
            return error_result(kQolErrInvalidArgument, "invalid registered bag source");
        }
        const int32_t bag_count = source->count();
        if (bag_count < 0) {
            return error_result(kQolErrInvalidArgument, "invalid registered bag count");
        }
        for (int32_t bag = 0; bag < bag_count; ++bag) {
            contracts::QolBagInfo info{};
            const QolResult info_result = source->info(bag, &info);
            if (!contracts::succeeded(info_result)) return info_result;
            if (info.capacity < 0 || info.slot_count < 0 || info.slot_count > info.capacity) {
                return error_result(kQolErrInvalidArgument, "invalid registered bag info");
            }

            out->append(",{\"space\":");
            *out += std::to_string(source->space_id);
            out->append(",\"bag\":");
            *out += std::to_string(bag);
            out->append(",\"capacity\":");
            *out += std::to_string(info.capacity);
            out->append(",\"used\":");
            *out += std::to_string(info.slot_count);
            out->append(",\"items\":[");

            bool first_item = true;
            for (int32_t slot = 0; slot < info.capacity; ++slot) {
                contracts::QolInventoryItemRef item_ref{};
                const QolResult item_result = source->item_at(bag, slot, &item_ref);
                if (!contracts::succeeded(item_result)) {
                    if (item_result.code == kQolErrNotFound) continue;
                    return item_result;
                }
                if (item_ref.space_id != source->space_id || item_ref.bag != bag ||
                    item_ref.slot != slot || item_ref.item.slot == 0 ||
                    item_ref.item.generation == 0) {
                    return error_result(kQolErrInvalidArgument, "invalid registered bag item");
                }
                if (!first_item) out->push_back(',');
                first_item = false;
                out->append("{\"space\":");
                *out += std::to_string(source->space_id);
                out->append(",\"bag\":");
                *out += std::to_string(bag);
                out->append(",\"slot\":");
                *out += std::to_string(slot);
                out->append(",\"item\":{\"slot\":");
                *out += std::to_string(item_ref.item.slot);
                out->append(",\"generation\":");
                *out += std::to_string(item_ref.item.generation);
                out->append("}}");
            }
            out->append("]}");
        }
    }
    return contracts::ok();
}

QolResult build_inventory_json(std::string* out) {
    if (out == nullptr || g_port == nullptr ||
        g_port->read.in_world == nullptr || g_port->read.item_ref_at == nullptr ||
        g_port->read.inventory_item_view == nullptr) {
        return error_result(kQolErrInvalidArgument, "inventory serializer unavailable");
    }
    int32_t in_world = 0;
    if (!contracts::succeeded(g_port->read.in_world(&in_world))) return not_ready();
    if (in_world == 0) {
        *out = kNotInGameBody;
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

    int32_t c18_bag_count = 0;
    if (contracts::qol_item_port_bag_space_size_valid(g_port) &&
        g_port->read.bag_count != nullptr) {
        const QolResult count_result =
            g_port->read.bag_count(contracts::kQolBagSpaceVariant, &c18_bag_count);
        if (!contracts::succeeded(count_result)) {
            if (count_result.code != kQolErrUnsupported && count_result.code != kQolErrNotReady) {
                return count_result;
            }
            c18_bag_count = 0;
        }
    }
    if (c18_bag_count < 0) c18_bag_count = 0;
    for (int32_t bag = 0; bag < c18_bag_count; ++bag) {
        if (g_port->read.bag_info == nullptr || g_port->read.item_at == nullptr) {
            return error_result(kQolErrUnsupported, "c18 bag read unavailable");
        }
        const contracts::QolBagRef ref{contracts::kQolBagSpaceVariant, bag};
        contracts::QolBagInfo info{};
        const QolResult info_result = g_port->read.bag_info(&ref, &info);
        if (!contracts::succeeded(info_result)) return info_result;
        if (info.capacity < 0 || info.slot_count < 0) {
            return error_result(kQolErrInvalidArgument, "invalid c18 bag info");
        }
        // 原始 item.state 保留扁平袋号，不新增 space 字段。
        json += ",{\"bag\":" + std::to_string(contracts::kQolOriginalBagCount + bag) +
                ",\"items\":[";
        int32_t filled = 0;
        for (int32_t slot = 0; slot < info.capacity; ++slot) {
            contracts::QolInventoryItemRef item_ref{};
            const QolResult ref_result = g_port->read.item_at(&ref, slot, &item_ref);
            if (!contracts::succeeded(ref_result)) {
                if (ref_result.code == kQolErrNotFound) continue;
                return ref_result;
            }
            if (item_ref.item.slot == 0 || item_ref.item.generation == 0) continue;
            contracts::QolItemInventoryView view{};
            const QolResult view_result =
                g_port->read.inventory_item_view(item_ref.item, &view);
            if (!contracts::succeeded(view_result)) {
                if (view_result.code == kQolErrNotFound) continue;
                return view_result;
            }
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
        json += "],\"capacity\":" + std::to_string(info.capacity) +
                ",\"slot_count\":" + std::to_string(info.slot_count) + "}";
    }
    const QolResult extension_result = append_registered_inventory_bags(&json);
    if (!contracts::succeeded(extension_result)) return extension_result;
    json += "]}";
    *out = std::move(json);
    return contracts::ok();
}

QolResult serialize_inventory_json(char* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid item state output");
    }
    *out_size = 0;
    std::string json;
    const QolResult result = build_inventory_json(&json);
    if (!contracts::succeeded(result)) return result;
    if (json.size() > std::numeric_limits<uint32_t>::max()) {
        return error_result(kQolErrInvalidArgument, "inventory json too large");
    }
    *out_size = static_cast<uint32_t>(json.size());
    if (out == nullptr) return contracts::ok();
    if (json.size() > capacity) {
        return error_result(kQolErrInvalidArgument, "inventory buffer too small");
    }
    std::memcpy(out, json.data(), json.size());
    return contracts::ok();
}

QolResult write_item_state_json(int32_t kind, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (out_size == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid item state output");
    }
    switch (static_cast<contracts::QolItemStateKind>(kind)) {
        case contracts::QolItemStateKind::kRawInventory:
            return serialize_inventory_json(out, capacity, out_size);
        case contracts::QolItemStateKind::kEnrichedInventory: {
            const auto write_error = [out, capacity, out_size](const char* error) {
                const std::string body = std::string("{\"ok\":false,\"error\":\"") + error +
                                         "\"}";
                if (out == nullptr) {
                    *out_size = static_cast<uint32_t>(body.size());
                    return contracts::ok();
                }
                if (body.size() > capacity) {
                    *out_size = static_cast<uint32_t>(body.size());
                    return error_result(kQolErrBlocked, "item state buffer too small");
                }
                std::memcpy(out, body.data(), body.size());
                *out_size = static_cast<uint32_t>(body.size());
                return contracts::ok();
            };
            std::string raw_inventory;
            const QolResult serialized = build_inventory_json(&raw_inventory);
            if (!contracts::succeeded(serialized)) return serialized;
            std::string body;
            if (raw_inventory.compare(0, 9, "{\"error\":") == 0) {
                body = std::move(raw_inventory);
            } else if (!enrich_inventory(raw_inventory.data(),
                                         static_cast<uint32_t>(raw_inventory.size()), g_port,
                                         &body)) {
                return write_error("internal error");
            }
            if (body.size() > std::numeric_limits<uint32_t>::max()) {
                return error_result(kQolErrBlocked, "item state json too large");
            }
            *out_size = static_cast<uint32_t>(body.size());
            if (out == nullptr) return contracts::ok();
            if (body.size() > capacity) {
                return error_result(kQolErrBlocked, "item state buffer too small");
            }
            std::memcpy(out, body.data(), body.size());
            return contracts::ok();
        }
        default:
            return error_result(kQolErrInvalidArgument, "invalid item state kind");
    }
}

void update_snapshot() {
    if (g_port == nullptr || g_port->read.money == nullptr || g_port->read.in_world == nullptr) {
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

    if (!contracts::succeeded(g_port->read.money(&next.money))) {
        log_snapshot_serialize_failure();
        return;
    }
    next.state = SnapshotState::kReady;
    g_snapshot.publish(next);
}

int32_t frame_tick(int64_t, void*) {
    (void)read_inventory();
    update_snapshot();
    shop::refresh_snapshot();
    return 1;
}
extern const contracts::ItemStateApi kItemStateApi{&write_item_state_json};

}  // namespace qol::features::core_item
