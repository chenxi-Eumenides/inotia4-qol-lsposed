#include "contracts/host_sdk.h"
#include "plugins/extension_bag/extension_bag_bridge.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/bag_state_store.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/status_snapshot.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int g_pass = 0;
int g_fail = 0;
int g_log_calls = 0;
int g_effective_count_calls = 0;
int g_view_count_calls = 0;
int g_world_checks = 0;
int g_live_item = 0;
qol::contracts::QolHostSdk g_sdk{};
qol::contracts::QolItemCountApi g_count_api{};

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (condition) {                                                        \
            ++g_pass;                                                           \
        } else {                                                                \
            ++g_fail;                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (false)

QolResult record_log(int32_t, int32_t, const char*, const char*, uint32_t) {
    ++g_log_calls;
    return qol::contracts::ok();
}

QolResult read_effective_count(const void* item, uint32_t* out_count) {
    ++g_effective_count_calls;
    *out_count = item == &g_live_item ? 81u : 0u;
    return qol::contracts::ok();
}

QolResult read_view_count(uint32_t canonical, int32_t, uint32_t* out_count) {
    ++g_view_count_calls;
    *out_count = canonical;
    return qol::contracts::ok();
}

}  // namespace

namespace qol::features::extension_bag {

const qol::contracts::QolHostSdk* extension_bag_host_sdk() { return &g_sdk; }

}  // namespace qol::features::extension_bag

bool bag_size_word_locked(int, uint32_t*) { return false; }

bool read_world_in_world() {
    ++g_world_checks;
    return true;
}

bool extension_bag_session_admitted_locked() { return true; }

const qol::contracts::QolItemCountApi* item_count_api() { return &g_count_api; }

void prepare_count_api() {
    g_count_api = {};
    g_count_api.effective = &read_effective_count;
    g_count_api.view_from_canonical = &read_view_count;
}

int main() {
    using qol::extension_bag::bag_snapshot;
    using qol::extension_bag::load_status_snapshot;
    using qol::extension_bag::publish_bag_snapshot;

    g_sdk.log.write = &record_log;
    prepare_count_api();
    auto& state = runtime_state();
    state.enabled.store(true);
    state.objects.bag.bag_count = 3;
    state.objects.bag.capacities = {12, 16, 8, 0, 0, 0, 0, 0};
    state.objects.bag.items[0][2].category = 222;
    state.objects.bag.items[0][2].count = 9;
    state.objects.bag.items[0][14].category = 444;  // 超出派生容量，不应输出。
    state.objects.bag.items[0][14].count = 4;
    state.objects.bag.items[1][5].category = 333;
    state.objects.bag.items[1][5].count = 7;
    state.objects.module_objects[0][2] = &g_live_item;
    state.persistence.revision.store(41);

    CHECK(!load_status_snapshot());
    CHECK(virtual_bag_inventory_bags_json().empty());
    CHECK(virtual_bag_inventory_bags_json().empty());
    CHECK(g_log_calls == 1);
    CHECK(g_world_checks == 0);

    const std::string not_ready_status = extension_bag_api_status_json_impl();
    CHECK(not_ready_status ==
          "{\"enabled\":true,\"injected\":false,\"extension_tab_button\":false,"
          "\"inventory_frame_active\":false,\"recovery_action\":\"none\",\"state\":{}}");
    CHECK(extension_bag_api_status_json_impl() == not_ready_status);
    CHECK(g_log_calls == 2);
    CHECK(g_world_checks == 0);

    int32_t capacity = -1;
    CHECK(!virtual_bag_logical_capacity(6, &capacity));
    CHECK(capacity == 0);
    CHECK(!virtual_bag_logical_capacity(5, &capacity));
    CHECK(capacity == 0);

    publish_bag_snapshot();
    const auto snapshot = load_status_snapshot();
    CHECK(snapshot != nullptr);
    CHECK(snapshot->effective_counts[0][2] == 81);
    CHECK(snapshot->effective_counts[1][5] == 7);
    CHECK(snapshot->effective_counts[0][14] == 0);
    CHECK(g_effective_count_calls == 1);
    CHECK(g_view_count_calls == 1);
    CHECK(extension_bag_api_status_json_impl() ==
          qol::extension_bag::status_snapshot_json(*snapshot));

    CHECK(virtual_bag_logical_capacity(6, &capacity));
    CHECK(capacity == snapshot->bag.capacities[0]);
    CHECK(virtual_bag_logical_capacity(7, &capacity));
    CHECK(capacity == snapshot->bag.capacities[1]);

    // 发布后再改 live State 和对象指针，查询仍只使用快照中的数据。
    state.objects.bag.items[0][2].count = 99;
    state.objects.bag.capacities[0] = 4;
    state.objects.module_objects[0][2] = nullptr;
    CHECK(virtual_bag_logical_capacity(6, &capacity));
    CHECK(capacity == snapshot->bag.capacities[0]);
    const uint64_t revision_before_query = state.persistence.revision.load();
    const std::string json = virtual_bag_inventory_bags_json();
    CHECK(json ==
          ",{\"bag\":6,\"items\":[{\"slot\":2,\"category\":222,\"count\":81}],\"capacity\":12,\"slot_count\":1}"
          ",{\"bag\":7,\"items\":[{\"slot\":5,\"category\":333,\"count\":7}],\"capacity\":16,\"slot_count\":1}"
          ",{\"bag\":8,\"items\":[],\"capacity\":8,\"slot_count\":0}");
    CHECK(g_effective_count_calls == 1);
    CHECK(g_view_count_calls == 1);
    CHECK(state.persistence.revision.load() == revision_before_query);
    CHECK(snapshot->revision == 41);
    CHECK(g_world_checks == 5);

    std::printf("extension-bag-inventory-snapshot: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
