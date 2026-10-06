// 持久配置桥（settings_config v1.0）host 测试：真实桥实现 + 受控 dispatch 脚本。
// 覆盖契约面：白名单/布尔校验、未就绪语义、排队准许、回执状态机、容量上限、
// 镜像 revision 单调、shutdown 后拒绝。JNI 面在 Android 构建内，host 不覆盖。
#include "host/settings_config_bridge.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
using namespace qol::contracts;
using namespace qol::host;
constexpr uint32_t kCount = kSettingsConfigBoolCount;

enum Mode { kGate = 0, kApply, kReject, kUnchanged, kDispatchFail };
std::atomic<int> g_mode{kGate};
std::atomic<bool> g_gate{false};
std::atomic<uint64_t> g_last_id{0};

template <typename Pred>
bool wait_for(Pred&& predicate, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

QolResult test_dispatch(void*, uint64_t id, const char* key, int32_t desired) {
    switch (g_mode.load()) {
    case kGate:
        wait_for([] { return g_gate.load(); });
        return settings_config_complete(id, 1, desired, SettingsConfigApplyState::kApplied, 0, "", "applied");
    case kApply:
        return settings_config_complete(id, 1, desired, SettingsConfigApplyState::kApplied, 0, "", "applied");
    case kReject:
        return settings_config_complete(id, 0, 0, SettingsConfigApplyState::kRejected, 22,
                                        "rejected by setter", "not applied");
    case kUnchanged:
        return settings_config_complete(id, 1, desired, SettingsConfigApplyState::kUnchanged, 0, "", "unchanged");
    default:
        return error(kQolErrInternal, "dispatch broken", 14);
    }
}

bool find_request(uint64_t id, SettingsConfigRequestStatus* status) {
    status->size = sizeof(SettingsConfigRequestStatus);
    return succeeded(settings_config_api().query_request(id, status));
}

void test_contract_surface() {
    const auto& api = settings_config_api();
    assert(api.size == sizeof(QolSettingsConfigApi));
    assert(api.major == kSettingsConfigMajor && api.minor == kSettingsConfigMinor);
    assert(api.read_bool != nullptr && api.request_set_bool != nullptr && api.query_request != nullptr);
    assert(std::strcmp(settings_config_key(0), "stackLimitIncrease") == 0);
    assert(std::strcmp(settings_config_key(9), "pushAllEnabled") == 0);
    assert(settings_config_key(10) == nullptr);
}

void test_validation_before_ready() {
    int32_t value = -1;
    assert(settings_config_api().read_bool(nullptr, &value).code == kQolErrInvalidArgument);
    assert(settings_config_api().read_bool("stackLimitIncrease", nullptr).code == kQolErrInvalidArgument);
    assert(settings_config_api().read_bool("unknownKey", &value).code == kQolErrNotFound);
    // 未发布镜像：公开键也必须 NotReady，不得回落默认值。
    assert(settings_config_api().read_bool("stackLimitIncrease", &value).code == kQolErrNotReady);
    uint64_t id = 0;
    assert(settings_config_api().request_set_bool("stackLimitIncrease", 1, &id).code == kQolErrNotReady);
    assert(settings_config_install(nullptr, nullptr).code == kQolErrInvalidArgument);
    assert(settings_config_api().request_set_bool(nullptr, 1, &id).code == kQolErrInvalidArgument);
    assert(settings_config_api().request_set_bool("stackLimitIncrease", 1, nullptr).code == kQolErrInvalidArgument);
    assert(settings_config_api().request_set_bool("stackLimitIncrease", 2, &id).code == kQolErrInvalidArgument);
    assert(settings_config_api().request_set_bool("unknownKey", 1, &id).code == kQolErrNotFound);
    assert(settings_config_publish(nullptr, kCount, 1).code == kQolErrInvalidArgument);
    int32_t mirror[kCount] = {1, 0, 1, 0, 1, 0, 1, 0, 0, 1};
    assert(settings_config_publish(mirror, kCount - 1, 1).code == kQolErrInvalidArgument);
    assert(settings_config_publish(mirror, kCount, 0).code == kQolErrInvalidArgument);
    int32_t bad[kCount] = {7, 0, 1, 0, 1, 0, 1, 0, 0, 1};
    assert(settings_config_publish(bad, kCount, 1).code == kQolErrInvalidArgument);
    assert(settings_config_complete(1, 1, 1, static_cast<SettingsConfigApplyState>(6), 0, "", "").code ==
           kQolErrInvalidArgument);
}

void test_mirror_and_revisions() {
    int32_t mirror[kCount] = {1, 0, 1, 0, 1, 0, 1, 0, 0, 1};
    assert(succeeded(settings_config_publish(mirror, kCount, 1)));
    int32_t value = 0;
    assert(succeeded(settings_config_api().read_bool("stackLimitIncrease", &value)));
    assert(value == 1);
    assert(succeeded(settings_config_api().read_bool("moveMergeEnabled", &value)));
    assert(value == 0);
    assert(settings_config_api().read_bool("apiEnabled", &value).ok == kQolOk && value == 1);
    // revision 单调：回退与重复都被拒绝，镜像保持旧值。
    assert(settings_config_publish(mirror, kCount, 1).code == kQolErrConflict);
    int32_t flipped[kCount] = {0, 1, 1, 0, 1, 0, 1, 0, 0, 1};
    assert(settings_config_publish(flipped, kCount, 0).code == kQolErrInvalidArgument);
    assert(succeeded(settings_config_publish(flipped, kCount, 2)));
    assert(succeeded(settings_config_api().read_bool("stackLimitIncrease", &value)));
    assert(value == 0 && succeeded(settings_config_api().read_bool("moveMergeEnabled", &value)) && value == 1);
}

void test_admission_capacity_and_receipts() {
    assert(succeeded(settings_config_install(&test_dispatch, nullptr)));
    // 重复安装同一 dispatch 幂等；不同 context 冲突。
    assert(succeeded(settings_config_install(&test_dispatch, nullptr)));
    assert(settings_config_install(&test_dispatch, reinterpret_cast<void*>(1)).code == kQolErrConflict);

    SettingsConfigRequestStatus status{};
    status.size = sizeof(status) - 1;
    assert(settings_config_api().query_request(1, &status).code == kQolErrInvalidArgument);
    status.size = sizeof(status);

    // gate 关闭：worker 停在首个 dispatch 内，后续请求停在排队态。
    uint64_t ids[kSettingsConfigRequestCapacity] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        assert(succeeded(settings_config_api().request_set_bool(i % 2 == 0 ? "stackLimitIncrease" : "opEnabled",
                                         i % 2 == 0 ? 1 : 0, &ids[i])));
        assert(ids[i] == i + 1);
        assert(find_request(ids[i], &status));
        assert(status.state == SettingsConfigRequestState::kQueued);
        assert(status.desired_value == (i % 2 == 0 ? 1 : 0));
        // 排队即预填当前确认值（非生效门禁值）：rev2 镜像 stackLimit=0 / opEnabled=1。
        assert(status.configured_value == (i % 2 == 0 ? 0 : 1));
        assert(status.persisted == 0);
    }
    assert(settings_config_api().query_request(0, &status).code == kQolErrInvalidArgument);
    assert(settings_config_api().query_request(9999, &status).code == kQolErrNotFound);

    // 填满 128 槽后容量拒绝；不影响既有排队请求。
    QolResult last{kQolOk, 0, nullptr, 0};
    for (uint32_t i = 3; i < kSettingsConfigRequestCapacity; ++i) {
        last = settings_config_api().request_set_bool("simpleModeEnabled", 1, &ids[i]);
        assert(succeeded(last));
    }
    uint64_t overflow = 0;
    assert(settings_config_api().request_set_bool("simpleModeEnabled", 1, &overflow).code == kQolErrBlocked);
    assert(overflow == 0);

    // 开闸：worker 顺序派发并完成全部请求。
    g_gate = true;
    assert(wait_for([&] {
        for (const auto id : ids) {
            if (!find_request(id, &status) || status.state != SettingsConfigRequestState::kSucceeded) return false;
        }
        return true;
    }));
    assert(std::strcmp(status.apply_message, "applied") == 0);
    assert(status.persisted == 1 && status.configured_value == 1);
    assert(status.apply_state == SettingsConfigApplyState::kApplied);
    // 完成后重复回执必须冲突。
    assert(settings_config_complete(ids[0], 1, 1, SettingsConfigApplyState::kApplied, 0, "", "").code ==
           kQolErrConflict);
}

void test_receipt_semantics() {
    SettingsConfigRequestStatus status{};
    status.size = sizeof(status);
    uint64_t id = 0;

    g_mode = kReject;
    assert(succeeded(settings_config_api().request_set_bool("apiEnabled", 0, &id)));
    g_last_id = id;
    assert(wait_for([&] { return find_request(id, &status) && status.state == SettingsConfigRequestState::kFailed; }));
    assert(status.persisted == 0 && status.configured_value == 0);
    assert(status.apply_state == SettingsConfigApplyState::kRejected && status.error_code == 22);
    assert(std::strcmp(status.error, "rejected by setter") == 0);

    // dispatch 自身失败由 worker 代填回执：persisted=0、kNotSubmitted，仍为 failed。
    g_mode = kDispatchFail;
    assert(succeeded(settings_config_api().request_set_bool("apiEnabled", 0, &id)));
    g_last_id = id;
    assert(wait_for([&] { return find_request(id, &status) && status.state == SettingsConfigRequestState::kFailed; }));
    assert(status.persisted == 0 && status.error_code == kQolErrInternal);
    assert(status.apply_state == SettingsConfigApplyState::kNotSubmitted);

    g_mode = kUnchanged;
    assert(succeeded(settings_config_api().request_set_bool("debugLogEnabled", 1, &id)));
    assert(wait_for([&] {
        return find_request(id, &status) && status.state == SettingsConfigRequestState::kSucceeded;
    }));
    assert(status.apply_state == SettingsConfigApplyState::kUnchanged && status.persisted == 1);
}

void test_shutdown_gate() {
    settings_config_shutdown();
    SettingsConfigRequestStatus status{};
    status.size = sizeof(status);
    uint64_t id = 0;
    int32_t value = 0;
    // 已发布镜像仍可读；写路径因 stopping 被拒；未驱逐的既有回执仍可查。
    assert(succeeded(settings_config_api().read_bool("stackLimitIncrease", &value)));
    assert(settings_config_api().request_set_bool("stackLimitIncrease", 1, &id).code == kQolErrNotReady);
    assert(settings_config_api().query_request(g_last_id.load(), &status).ok == kQolOk);
}
}  // namespace

int main() {
    test_contract_surface();
    test_validation_before_ready();
    test_mirror_and_revisions();
    test_admission_capacity_and_receipts();
    test_receipt_semantics();
    test_shutdown_gate();
    std::puts("settings_config_bridge: PASS (contract surface, not-ready, mirror revisions, capacity, receipts, shutdown)");
}
