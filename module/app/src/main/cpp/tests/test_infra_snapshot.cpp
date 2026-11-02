// infra/snapshot.h host 单测：只读快照的发布/读取语义与并发一致性。
// 并发断言：写者持续发布关联字段 (a, b) = (2f, 2f+1)，读者不得观察到撕裂组合。

#include "infra/snapshot.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

struct Pod {
    uint32_t a;
    uint32_t b;
    uint32_t frame;
};

static void test_roundtrip() {
    qol::infra::Snapshot<Pod> snap;
    Pod out{};

    CHECK(!snap.valid());
    CHECK(!snap.read(out));  // 未发布：读失败

    snap.publish(Pod{1, 2, 3});
    CHECK(snap.valid());
    CHECK(snap.read(out));
    CHECK(out.a == 1 && out.b == 2 && out.frame == 3);

    snap.publish(Pod{4, 5, 6});
    CHECK(snap.read(out));
    CHECK(out.a == 4 && out.b == 5 && out.frame == 6);
    CHECK(snap.sequence() == 4);  // 每次发布 +2
}

static void test_concurrent_consistency() {
    qol::infra::Snapshot<Pod> snap;
    snap.publish(Pod{0, 1, 0});

    std::atomic<bool> stop{false};
    std::atomic<bool> torn{false};
    std::atomic<uint32_t> reads{0};

    std::thread writer([&] {
        for (uint32_t frame = 1; frame <= 200000; ++frame) {
            snap.publish(Pod{frame * 2, frame * 2 + 1, frame});
        }
        stop.store(true, std::memory_order_release);
    });

    Pod out{};
    while (!stop.load(std::memory_order_acquire)) {
        if (snap.read(out)) {
            ++reads;
            if (out.b != out.a + 1) torn.store(true, std::memory_order_relaxed);
        }
    }
    writer.join();

    CHECK(reads.load() > 0);
    CHECK(!torn.load());
}

int main() {
    test_roundtrip();
    test_concurrent_consistency();
    std::printf("infra_snapshot: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
