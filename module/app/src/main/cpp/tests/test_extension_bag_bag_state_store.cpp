#include "plugins/extension_bag/runtime/bag_state_store.h"
#include "plugins/extension_bag/runtime/state.h"

#include <cstdio>

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

    auto& state = ::runtime_state();
    BagSnapshot initial{};
    CHECK(!bag_snapshot(&initial));

    state.objects.bag.types = {4, 4, 3, 0, 0};
    state.objects.bag.capacities = {16, 16, 12, 0, 0};
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

    std::printf("bag-state-store: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
