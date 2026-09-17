// infra/capabilities.h 与 infra/status.h host 单测：
// 能力注册表的版本兼容/生命周期，功能状态登记表的状态/快照与名称映射语义。
//
// 说明：capabilities.h 与 status.h 声明的是同一个 qol::infra::reset_for_test()
// （签名一致 → 同一符号），因此共享重置只定义一次（capabilities.cpp）并同时清空
// 两个注册表；单模块隔离由 capabilities_reset_for_test()/status_reset_for_test()
// 两个不写入冻结头文件的辅助入口验证。

#include "infra/capabilities.h"
#include "infra/test_reset.h"
#include "infra/status.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace qol::infra {
// 冻结头文件之外的模块级测试重置入口。
void capabilities_reset_for_test();
void status_reset_for_test();
}  // namespace qol::infra

using qol::infra::capability_count;
using qol::infra::CapabilityProvision;
using qol::infra::FeatureState;
using qol::infra::FeatureStatus;
using qol::infra::feature_state_name;
using qol::infra::register_capability;
using qol::infra::require_capability;
using qol::infra::reset_for_test;
using qol::infra::status_count;
using qol::infra::status_register;
using qol::infra::status_set_degraded;
using qol::infra::status_snapshot;
using qol::infra::status_update;
using qol::infra::unregister_capability;

// 1. 版本兼容：major 必须相等，提供方 minor ≥ 消费者要求。
static void test_capability_version_compat() {
    reset_for_test();
    static int iface = 0;

    CHECK(register_capability(CapabilityProvision{"caps.version", QolVersion{1, 5}, &iface}));
    // major 相等且 minor 满足（含正好相等）。
    CHECK(require_capability("caps.version", QolVersionRange{1, 3}) == &iface);
    CHECK(require_capability("caps.version", QolVersionRange{1, 5}) == &iface);
    // major 不等：拒绝。
    CHECK(require_capability("caps.version", QolVersionRange{2, 0}) == nullptr);
    CHECK(require_capability("caps.version", QolVersionRange{0, 5}) == nullptr);
    // 要求 minor 高于提供方：拒绝。
    CHECK(require_capability("caps.version", QolVersionRange{1, 6}) == nullptr);
    // 提供方 minor 更高：满足较低要求。
    CHECK(register_capability(CapabilityProvision{"caps.higher", QolVersion{1, 9}, &iface}));
    CHECK(require_capability("caps.higher", QolVersionRange{1, 2}) == &iface);
    // 未注册 id：nullptr。
    CHECK(require_capability("caps.missing", QolVersionRange{1, 0}) == nullptr);
    CHECK(require_capability(nullptr, QolVersionRange{1, 0}) == nullptr);
}

// 2. 注册/注销生命周期：非法参数、重复注册、撤销幂等。
static void test_capability_lifecycle() {
    reset_for_test();
    static int iface = 0;

    CHECK(!register_capability(CapabilityProvision{nullptr, QolVersion{1, 0}, &iface}));
    CHECK(!register_capability(CapabilityProvision{"caps.nulliface", QolVersion{1, 0}, nullptr}));
    CHECK(capability_count() == 0);

    CHECK(register_capability(CapabilityProvision{"caps.a", QolVersion{1, 0}, &iface}));
    CHECK(capability_count() == 1);
    // 同 id 重复注册（未撤销）拒绝，且不覆盖原实现。
    CHECK(!register_capability(CapabilityProvision{"caps.a", QolVersion{9, 9}, &iface}));
    CHECK(capability_count() == 1);
    CHECK(require_capability("caps.a", QolVersionRange{9, 9}) == nullptr);

    // 撤销后 require 为空，且可重新注册。
    CHECK(unregister_capability("caps.a"));
    CHECK(capability_count() == 0);
    CHECK(require_capability("caps.a", QolVersionRange{1, 0}) == nullptr);
    CHECK(register_capability(CapabilityProvision{"caps.a", QolVersion{1, 0}, &iface}));

    // 撤销幂等：重复撤销/未知 id/nullptr 均返回 false。
    CHECK(unregister_capability("caps.a"));
    CHECK(!unregister_capability("caps.a"));
    CHECK(!unregister_capability("caps.unknown"));
    CHECK(!unregister_capability(nullptr));
}

// 3. 容量上限 64：满后第 65 条拒绝。
static char g_cap_ids[65][16];

static void test_capability_capacity() {
    reset_for_test();
    static int iface = 0;

    for (uint32_t i = 0; i < 64; ++i) {
        std::snprintf(g_cap_ids[i], sizeof(g_cap_ids[i]), "caps.%u", i);
        CHECK(register_capability(CapabilityProvision{g_cap_ids[i], QolVersion{1, 0}, &iface}));
    }
    CHECK(capability_count() == 64);
    std::snprintf(g_cap_ids[64], sizeof(g_cap_ids[64]), "caps.64");
    CHECK(!register_capability(CapabilityProvision{g_cap_ids[64], QolVersion{1, 0}, &iface}));
    CHECK(capability_count() == 64);
    // 满时重复 id 判定仍生效。
    CHECK(!register_capability(CapabilityProvision{g_cap_ids[0], QolVersion{1, 0}, &iface}));
}

// 4. 状态登记：注册/重复/未注册更新、error 设置与清除、enabled 切换。
static void test_status_lifecycle() {
    reset_for_test();
    CHECK(status_count() == 0);

    CHECK(!status_register(nullptr, "1.0"));
    CHECK(status_register("feat.a", "1.2.0"));
    CHECK(status_count() == 1);
    // 同 id 重复注册拒绝。
    CHECK(!status_register("feat.a", "9.9.9"));
    CHECK(status_count() == 1);
    // version 允许 nullptr。
    CHECK(status_register("feat.plain", nullptr));

    // 初始状态：kRegistered / disabled / 无错误。
    FeatureStatus out[4]{};
    uint32_t written = 0;
    CHECK(status_snapshot(out, 4, &written));
    CHECK(written == 2);
    CHECK(std::strcmp(out[0].id, "feat.a") == 0);
    CHECK(std::strcmp(out[0].version, "1.2.0") == 0);
    CHECK(out[0].state == FeatureState::kRegistered);
    CHECK(!out[0].enabled);
    CHECK(out[0].error == nullptr);
    CHECK(out[1].version == nullptr);

    // 未注册（含 nullptr）更新拒绝。
    CHECK(!status_update("feat.missing", FeatureState::kEnabled, true, nullptr));
    CHECK(!status_update(nullptr, FeatureState::kEnabled, true, nullptr));

    // 更新 state/enabled/error。
    CHECK(status_update("feat.a", FeatureState::kEnabled, true, "boom"));
    CHECK(status_snapshot(out, 4, &written));
    CHECK(out[0].state == FeatureState::kEnabled);
    CHECK(out[0].enabled);
    CHECK(out[0].error != nullptr && std::strcmp(out[0].error, "boom") == 0);

    // error 传 nullptr 表示清除。
    CHECK(status_update("feat.a", FeatureState::kInstalledDisabled, false, nullptr));
    CHECK(status_snapshot(out, 4, &written));
    CHECK(out[0].state == FeatureState::kInstalledDisabled);
    CHECK(!out[0].enabled);
    CHECK(out[0].error == nullptr);
}

// 4b. degraded 为正交健康标记：可与任意生命周期态同时存在，快照可读出。
static void test_status_degraded_flag() {
    reset_for_test();
    CHECK(status_register("d.a", "1.0.0"));

    FeatureStatus out[4]{};
    uint32_t written = 0;
    CHECK(status_snapshot(out, 4, &written));
    CHECK(!out[0].degraded);  // 注册初值 false

    CHECK(status_set_degraded("d.a", true));
    CHECK(status_snapshot(out, 4, &written));
    CHECK(out[0].degraded);
    // 生命周期态未被 degraded 影响。
    CHECK(out[0].state == FeatureState::kRegistered);

    CHECK(status_update("d.a", FeatureState::kEnabled, true, nullptr));
    CHECK(status_set_degraded("d.a", false));
    CHECK(status_snapshot(out, 4, &written));
    CHECK(!out[0].degraded);
    CHECK(out[0].state == FeatureState::kEnabled);

    // 未注册（含 nullptr）拒绝。
    CHECK(!status_set_degraded("d.missing", true));
    CHECK(!status_set_degraded(nullptr, true));
}

// 5. 快照：注册顺序、容量截断、非法参数。
static void test_status_snapshot() {
    reset_for_test();
    CHECK(status_register("s.0", nullptr));
    CHECK(status_register("s.1", nullptr));
    CHECK(status_register("s.2", nullptr));

    FeatureStatus out[8]{};
    uint32_t written = 0;
    CHECK(status_snapshot(out, 8, &written));
    CHECK(written == 3);
    CHECK(std::strcmp(out[0].id, "s.0") == 0);
    CHECK(std::strcmp(out[1].id, "s.1") == 0);
    CHECK(std::strcmp(out[2].id, "s.2") == 0);

    // capacity 小于条数：写出 capacity 条，written == capacity。
    written = 0;
    CHECK(status_snapshot(out, 2, &written));
    CHECK(written == 2);
    CHECK(std::strcmp(out[0].id, "s.0") == 0);
    CHECK(std::strcmp(out[1].id, "s.1") == 0);

    // capacity 0：成功但写出 0 条。
    written = 99;
    CHECK(status_snapshot(out, 0, &written));
    CHECK(written == 0);

    // 非法参数。
    CHECK(!status_snapshot(nullptr, 4, &written));
    CHECK(!status_snapshot(out, 4, nullptr));
}

// 6. feature_state_name：10 个生命周期态全部稳定命名，越界返回 unknown。
static void test_feature_state_names() {
    const FeatureState all[] = {
        FeatureState::kRegistered,        FeatureState::kResolving,
        FeatureState::kBlocked,           FeatureState::kUnsupported,
        FeatureState::kInvalid,           FeatureState::kInstalledDisabled,
        FeatureState::kEnabled,           FeatureState::kDeactivating,
        FeatureState::kInactive,          FeatureState::kFailed,
    };
    const char* expected[] = {
        "registered",        "resolving", "blocked",  "unsupported", "invalid",
        "installed_disabled", "enabled",  "deactivating", "inactive", "failed",
    };
    for (int i = 0; i < 10; ++i) {
        const char* name = feature_state_name(all[i]);
        CHECK(name != nullptr);
        CHECK(std::strcmp(name, "unknown") != 0);
        CHECK(std::strcmp(name, expected[i]) == 0);
    }
    CHECK(std::strcmp(feature_state_name(static_cast<FeatureState>(-1)), "unknown") == 0);
    CHECK(std::strcmp(feature_state_name(static_cast<FeatureState>(10)), "unknown") == 0);
    CHECK(std::strcmp(feature_state_name(static_cast<FeatureState>(100)), "unknown") == 0);
}

// 7. 两个注册表相互独立：单模块重置不影响另一模块；共享 reset 同时清空两者。
static void test_reset_isolation() {
    reset_for_test();
    static int iface = 0;

    CHECK(register_capability(CapabilityProvision{"caps.iso", QolVersion{1, 0}, &iface}));
    CHECK(status_register("feat.iso", "1.0"));
    CHECK(capability_count() == 1);
    CHECK(status_count() == 1);

    // 只重置状态：能力不受影响。
    qol::infra::status_reset_for_test();
    CHECK(status_count() == 0);
    CHECK(capability_count() == 1);
    CHECK(require_capability("caps.iso", QolVersionRange{1, 0}) == &iface);

    // 只重置能力：状态不受影响。
    CHECK(status_register("feat.iso2", "1.0"));
    qol::infra::capabilities_reset_for_test();
    CHECK(capability_count() == 0);
    CHECK(status_count() == 1);
    CHECK(require_capability("caps.iso", QolVersionRange{1, 0}) == nullptr);

    // 共享 reset_for_test 同时清空两者。
    reset_for_test();
    CHECK(capability_count() == 0);
    CHECK(status_count() == 0);
}

int main() {
    test_capability_version_compat();
    test_capability_lifecycle();
    test_capability_capacity();
    test_status_lifecycle();
    test_status_degraded_flag();
    test_status_snapshot();
    test_feature_state_names();
    test_reset_isolation();
    std::printf("infra_capabilities_status: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
