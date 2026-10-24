#include "plugins/extension_bag/runtime/bag_state_store.h"
#include "plugins/extension_bag/runtime/state.h"
#include "plugins/extension_bag/runtime/status_snapshot.h"

#include <array>
#include <cstdio>
#include <string>

namespace {

std::array<uint32_t, 5> g_original_capacity_words{{31, 42, 53, 64, 75}};
int g_fallback_calls = 0;

std::string status_fallback() {
    ++g_fallback_calls;
    return "fallback-status";
}

}  // namespace

bool bag_size_word_locked(int bag, uint32_t* out_word) {
    if (bag < 0 || bag >= static_cast<int>(g_original_capacity_words.size()) || out_word == nullptr) {
        return false;
    }
    *out_word = g_original_capacity_words[static_cast<size_t>(bag)];
    return true;
}

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (condition) {                                                        \
            ++g_pass;                                                           \
        } else {                                                                \
            ++g_fail;                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (false)

}  // namespace

int main() {
    using qol::extension_bag::BagSnapshot;
    using qol::extension_bag::bag_snapshot;
    using qol::extension_bag::publish_bag_snapshot;
    using qol::extension_bag::status_snapshot_json_or_fallback;
    using qol::extension_bag::load_status_snapshot;

    auto& state = ::runtime_state();
    bool used_fallback = false;
    CHECK(!load_status_snapshot());
    const std::string fallback_json =
        status_snapshot_json_or_fallback(status_fallback, &used_fallback);
    CHECK(used_fallback);
    CHECK(fallback_json == "fallback-status");
    CHECK(g_fallback_calls == 1);

    BagSnapshot initial{};
    CHECK(!bag_snapshot(&initial));

    qol::contracts::QolGameHost game{};
    qol::contracts::QolExtensionBagWritePort write_port{};
    uint8_t state_entry = 0;
    int tab_button = 0;
    state.host.game = &game;
    state.host.write_port = &write_port;
    state.enabled.store(true);
    state.persistence.state_entry = &state_entry;
    state.persistence.save_panel_hook_installed = true;
    state.ui.extension_tab_buttons[0] = &tab_button;
    state.ui.inventory_frame_active = true;
    state.objects.bag.types = {4, 4, 3, 0, 0};
    state.objects.bag.capacities = {16, 16, 12, 0, 0};
    state.objects.bag.bag_count = 3;
    state.objects.bag.items[1][2].category = 1234;
    state.objects.bag.items[1][2].count = 7;
    state.objects.bag.mode = virtual_bag::Mode::kModule;
    state.objects.bag.selected = 2;
    state.objects.bag.inspected = 5;
    state.persistence.revision.store(7);
    state.ui.inventory_generation = 11;
    publish_bag_snapshot();

    BagSnapshot published{};
    CHECK(bag_snapshot(&published));
    CHECK(published.types[0] == 4 && published.types[2] == 3);
    CHECK(published.capacities[1] == 16 && published.capacities[2] == 12);
    CHECK(published.mode == static_cast<int32_t>(virtual_bag::Mode::kModule));
    CHECK(published.selected == 2);
    CHECK(published.inspected == 5);
    CHECK(published.revision == 7);
    CHECK(published.inventory_generation == 11);

    auto status = load_status_snapshot();
    CHECK(status != nullptr);
    CHECK(status->bag.items[1][2].category == 1234 && status->bag.items[1][2].count == 7);
    CHECK(status->revision == 7);
    CHECK(status->enabled && status->injected && status->save_panel_hook_installed);
    CHECK(status->extension_tab_button && status->inventory_frame_active);
    CHECK(status->original_capacities[0] == 31 && status->original_capacities[4] == 75);

    const std::string status_json =
        status_snapshot_json_or_fallback(status_fallback, &used_fallback);
    CHECK(!used_fallback);
    CHECK(status_json.find("\"save_panel_hook\":true") != std::string::npos);
    CHECK(status_json.find("\"bagCount\":3") != std::string::npos);
    CHECK(status_json.find("\"originalCapacities\":[31,42,53,64,75]") != std::string::npos);
    CHECK(status_json.find("\"category\":1234,\"count\":7") != std::string::npos);
    CHECK(g_fallback_calls == 1);

    // 未重新发布前，快照保持不可变。
    state.objects.bag.selected = 4;
    state.objects.bag.capacities[0] = 0;
    state.persistence.revision.store(8);
    BagSnapshot stale{};
    CHECK(bag_snapshot(&stale));
    CHECK(stale.selected == 2);
    CHECK(stale.capacities[0] == 16);
    CHECK(stale.revision == 7);

    publish_bag_snapshot();
    BagSnapshot fresh{};
    CHECK(bag_snapshot(&fresh));
    CHECK(fresh.selected == 4);
    CHECK(fresh.capacities[0] == 0);
    CHECK(fresh.revision == 8);

    state.objects.bag.items[1][2].category = 4321;
    state.persistence.revision.store(9);
    publish_bag_snapshot();
    const uint64_t revision_before_query = state.persistence.revision.load();
    const std::string updated_status_json =
        status_snapshot_json_or_fallback(status_fallback, &used_fallback);
    CHECK(!used_fallback);
    CHECK(updated_status_json.find("\"category\":4321,\"count\":7") != std::string::npos);
    CHECK(load_status_snapshot()->revision == 9);
    CHECK(state.persistence.revision.load() == revision_before_query);
    CHECK(g_fallback_calls == 1);

    std::printf("bag-state-store/status-snapshot: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
