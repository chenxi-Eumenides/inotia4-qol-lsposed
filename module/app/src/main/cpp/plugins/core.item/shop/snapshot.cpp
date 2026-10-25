#include "plugins/core.item/shop/snapshot.h"

#include <vector>

#include "plugins/core.item/runtime/core_item_internal.h"

namespace qol::features::core_item::shop {
namespace {

uint64_t g_epoch = 0;

void publish(ShopSnapshot& snapshot) {
    snapshot.epoch = ++g_epoch;
    if (!g_snapshot.publish(snapshot)) log_snapshot_serialize_failure();
}

}  // namespace

Snapshot<ShopSnapshot> g_snapshot;

bool initialize_snapshot(const contracts::QolHostSdk* sdk) {
    if (!g_snapshot.initialize(sdk)) return false;
    g_epoch = 0;
    clear_snapshot();
    return true;
}

void clear_snapshot() {
    ShopSnapshot empty{};
    g_epoch = 0;
    (void)g_snapshot.publish(empty);
}

void destroy_snapshot() {
    g_snapshot.destroy();
    g_epoch = 0;
}

void refresh_snapshot() {
    if (g_port == nullptr || g_port->read.shop_items_json == nullptr ||
        g_port->read.in_world == nullptr) {
        log_snapshot_serialize_failure();
        return;
    }
    if (g_port->read.static_ready != nullptr) {
        int32_t ready = 0;
        if (!contracts::succeeded(g_port->read.static_ready(&ready))) {
            log_snapshot_serialize_failure();
            return;
        }
        if (ready == 0) return;
    }

    int32_t in_world = 0;
    if (!contracts::succeeded(g_port->read.in_world(&in_world))) {
        log_snapshot_serialize_failure();
        return;
    }

    ShopSnapshot next{};
    if (in_world == 0) {
        next.state = SnapshotState::kNotInGame;
        if (!copy_buffer(next.json, kSnapshotCapacity, &next.json_size, kNotInGameBody,
                         static_cast<uint32_t>(sizeof(kNotInGameBody) - 1))) {
            log_snapshot_serialize_failure();
            return;
        }
        publish(next);
        return;
    }

    std::vector<char> json(kSnapshotCapacity);
    uint32_t json_size = 0;
    if (!contracts::succeeded(g_port->read.shop_items_json(
            json.data(), static_cast<uint32_t>(json.size()), &json_size)) ||
        !copy_buffer(next.json, kSnapshotCapacity, &next.json_size, json.data(), json_size)) {
        log_snapshot_serialize_failure();
        return;
    }
    next.state = SnapshotState::kReady;
    publish(next);
}

}  // namespace qol::features::core_item::shop
