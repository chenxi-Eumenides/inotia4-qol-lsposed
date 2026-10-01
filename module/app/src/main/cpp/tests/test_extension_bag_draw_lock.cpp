#include "plugins/extension_bag/model/draw_state_lock.h"

#include <cstdio>
#include <mutex>

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

void test_draw_end_can_read_inventory_state_while_holding_runtime_lock() {
    std::mutex runtime_mutex;
    bool snapshot_callback_reentered = false;
    bool draw_body_holds_lock = false;

    qol::features::extension_bag::model::with_snapshot_then_lock(
        runtime_mutex,
        [&]() {
            // The game-state provider is synchronous and may re-enter EB; it must run
            // before draw-end acquires the non-recursive runtime mutex.
            const std::lock_guard<std::mutex> callback_lock(runtime_mutex);
            snapshot_callback_reentered = true;
            return 5;
        },
        [&](int state) {
            CHECK(state == 5);
            draw_body_holds_lock = !runtime_mutex.try_lock();
            if (!draw_body_holds_lock) runtime_mutex.unlock();
        });

    CHECK(snapshot_callback_reentered);
    CHECK(draw_body_holds_lock);
}

}  // namespace

int main() {
    test_draw_end_can_read_inventory_state_while_holding_runtime_lock();
    std::printf("extension_bag_draw_lock_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
