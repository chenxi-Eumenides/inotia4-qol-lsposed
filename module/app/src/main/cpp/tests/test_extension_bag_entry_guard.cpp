#include "plugins/extension_bag/runtime/entry_guard.h"
#include "plugins/extension_bag/runtime/state.h"

#include <cstdint>
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

int32_t g_main_thread = 1;
int32_t is_main_thread_stub() { return g_main_thread; }

void install_host() {
    static qol::contracts::QolGameHost game{};
    static qol::contracts::QolHostSdk sdk{};
    game.thread.is_main_thread = &is_main_thread_stub;
    auto& state = ::runtime_state();
    state.host.game = &game;
    state.host.sdk = &sdk;
}

}  // namespace

int main() {
    using qol::extension_bag::EntryGuard;
    using qol::extension_bag::ReentryPolicy;
    using qol::extension_bag::extension_bag_entry_stats;

    install_host();
    g_main_thread = 1;
    const auto base = extension_bag_entry_stats();

    {
        EntryGuard outer("test_outer", ReentryPolicy::kRejectWrite);
        CHECK(outer.entered());
        CHECK(qol::extension_bag::extension_bag_entry_active());
        {
            EntryGuard inner_write("test_inner_write", ReentryPolicy::kRejectWrite);
            CHECK(!inner_write.entered());
        }
        {
            EntryGuard inner_read("test_inner_read", ReentryPolicy::kReadSnapshot);
            CHECK(!inner_read.entered());
        }
    }
    CHECK(!qol::extension_bag::extension_bag_entry_active());
    const auto after_reentry = extension_bag_entry_stats();
    CHECK(after_reentry.write_reentries == base.write_reentries + 1);
    CHECK(after_reentry.read_reentries == base.read_reentries + 1);
    CHECK(after_reentry.main_thread_violations == base.main_thread_violations);

    // 非主线程入口必须被拒绝并计数。
    g_main_thread = 0;
    {
        EntryGuard off_main("test_off_main", ReentryPolicy::kRejectWrite);
        CHECK(!off_main.entered());
    }
    const auto after_off_main = extension_bag_entry_stats();
    CHECK(after_off_main.main_thread_violations == base.main_thread_violations + 1);

    // 回到主线程后可正常进入。
    g_main_thread = 1;
    {
        EntryGuard normal("test_normal", ReentryPolicy::kRejectWrite);
        CHECK(normal.entered());
    }

    std::printf("entry-guard: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
