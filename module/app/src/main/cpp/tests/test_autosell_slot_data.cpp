#include "plugins/autosell/config.h"
#include "plugins/autosell/scan.h"
#include "plugins/autosell/store.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace qol::features::autosell {
const qol::contracts::QolHostSdk* host_sdk();
}

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using namespace qol::contracts;

struct StoredSection {
    bool present = false;
    uint16_t version = 0;
    std::string bytes;
};

std::array<StoredSection, 3> g_disk{};
QolSlotDataSpec g_spec{};
QolSlotDataHandle g_handle = 0;
QolSlotDataHandle g_next_handle = 40;
QolSlotDataRequest g_next_request = 90;
QolSlotDataRequest g_pending_request = 0;
QolSlotDataStatus g_last_status{};
QolSlotSession g_session{-1, static_cast<uint32_t>(QolSlotSessionState::kUnavailable), 0};
QolHostSdk g_sdk{};
bool g_registered = false;
bool g_ready = false;
bool g_fail_next_write = false;
bool g_hold_next_write = false;
int g_write_count = 0;
int g_callback_count = 0;
int g_request_count = 0;
QolFrameHandler g_retry_handler = nullptr;
void* g_retry_context = nullptr;
uint64_t g_retry_task_id = 700;
uint64_t g_last_request_id = 0;
QolSlotDataContext g_pending_context{};
std::string g_pending_payload;
bool g_submit_new_revision_in_request = false;
bool g_fail_pending_during_unregister = false;
autosell::Config g_new_revision{};

QolResult failure(int code, const char* message) {
    return error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult fake_register(const QolSlotDataSpec* spec, QolSlotDataHandle* out_handle,
                        QolSlotDataRequest* initial) {
    if (spec == nullptr || out_handle == nullptr || initial == nullptr)
        return failure(kQolErrInvalidArgument, "invalid register");
    g_spec = *spec;
    g_handle = ++g_next_handle;
    *out_handle = g_handle;
    *initial = 0;
    g_registered = true;
    CHECK(std::strcmp(spec->section, "autosell") == 0);
    CHECK(spec->section_version == 2);
    CHECK(spec->read_policy == static_cast<uint32_t>(QolSlotDataReadPolicy::kSessionBound));
    CHECK(spec->write_policy == static_cast<uint32_t>(QolSlotDataWritePolicy::kImmediate));
    return ok();
}

void settle(QolSlotDataRequest request, QolResult result, uint32_t state) {
    g_last_status = {};
    g_last_status.size = sizeof(g_last_status);
    g_last_status.state = state;
    g_last_status.slot = g_session.slot;
    g_last_status.epoch = g_session.epoch;
    g_last_status.result = result;
    g_last_request_id = request;
    if (g_pending_request == request) g_pending_request = 0;
}

void finish_write(QolSlotDataRequest request) {
    if (!g_registered || !g_ready || request == 0) return;
    const bool captured_while_pending = request == g_pending_request;
    QolSlotDataContext ctx = captured_while_pending
                                 ? g_pending_context
                                 : QolSlotDataContext{g_session,
                                     static_cast<uint32_t>(QolSlotDataReason::kRequested), 0};
    std::string payload;
    if (captured_while_pending) {
        payload = g_pending_payload;
    } else {
        std::array<uint8_t, 4096> buffer{};
        uint64_t size = 0;
        ++g_callback_count;
        const QolResult captured = g_spec.capture(g_spec.user, &ctx, buffer.data(), buffer.size(), &size);
        if (!succeeded(captured) || size > buffer.size()) {
            settle(request, captured, static_cast<uint32_t>(QolSlotDataRequestState::kFailed));
            return;
        }
        payload.assign(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(size));
    }
    auto& section = g_disk[static_cast<size_t>(ctx.session.slot)];
    section.present = true;
    section.version = g_spec.section_version;
    section.bytes = payload;
    ++g_write_count;
    ++g_callback_count;
    g_spec.complete(g_spec.user, &ctx, static_cast<uint32_t>(QolSlotDataOutcome::kCommitted));
    settle(request, ok(), static_cast<uint32_t>(QolSlotDataRequestState::kCompleted));
}

QolResult fake_request_write(QolSlotDataHandle handle, uint64_t epoch,
                             QolSlotDataRequest* out_request) {
    if (out_request == nullptr) return failure(kQolErrInvalidArgument, "invalid request output");
    *out_request = 0;
    if (!g_registered || handle != g_handle || !g_ready || epoch != g_session.epoch)
        return failure(kQolErrNotReady, "session not ready");
    if (g_fail_next_write) {
        g_fail_next_write = false;
        return failure(kQolErrBlocked, "injected write request failure");
    }
    ++g_request_count;
    const QolSlotDataRequest request = ++g_next_request;
    *out_request = request;
    if (g_hold_next_write) {
        g_hold_next_write = false;
        g_pending_context = {g_session, static_cast<uint32_t>(QolSlotDataReason::kRequested), 0};
        std::array<uint8_t, 4096> buffer{};
        uint64_t size = 0;
        ++g_callback_count;
        const QolResult captured = g_spec.capture(g_spec.user, &g_pending_context, buffer.data(),
                                                  buffer.size(), &size);
        if (!succeeded(captured) || size > buffer.size()) return captured;
        g_pending_payload.assign(reinterpret_cast<const char*>(buffer.data()),
                                 static_cast<size_t>(size));
        g_pending_request = request;
        g_last_status = {};
        g_last_status.size = sizeof(g_last_status);
        g_last_status.state = static_cast<uint32_t>(QolSlotDataRequestState::kPending);
        if (g_submit_new_revision_in_request) {
            g_submit_new_revision_in_request = false;
            CHECK(autosell_store_commit_config(g_new_revision));
        }
        return ok();
    }
    finish_write(request);
    return ok();
}

QolResult fake_scheduler_add(int32_t, QolFrameHandler handler, void* ctx,
                             int32_t interval, int32_t, uint64_t* out_id) {
    if (handler == nullptr || out_id == nullptr || interval != 60)
        return failure(kQolErrInvalidArgument, "invalid retry task");
    g_retry_handler = handler;
    g_retry_context = ctx;
    *out_id = ++g_retry_task_id;
    return ok();
}

QolResult fake_scheduler_remove(uint64_t id) {
    if (id != g_retry_task_id) return failure(kQolErrNotFound, "retry task not found");
    g_retry_handler = nullptr;
    g_retry_context = nullptr;
    return ok();
}

void run_retry_frame(int64_t frame) {
    if (g_retry_handler != nullptr) (void)g_retry_handler(frame, g_retry_context);
}

void fail_pending_write(QolSlotDataRequest request) {
    settle(request, failure(kQolErrInternal, "injected terminal failure"),
           static_cast<uint32_t>(QolSlotDataRequestState::kFailed));
}

QolResult fake_unregister(QolSlotDataHandle handle, QolSlotDataRequest* out_request) {
    if (handle != g_handle || out_request == nullptr || !g_registered)
        return failure(kQolErrNotFound, "handle not found");
    ++g_callback_count;
    const QolResult detached = g_spec.prepare_detach(g_spec.user, &g_session, 0);
    if (!succeeded(detached)) return detached;
    if (g_pending_request != 0) {
        const uint32_t terminal_state = g_fail_pending_during_unregister
                                            ? static_cast<uint32_t>(QolSlotDataRequestState::kFailed)
                                            : static_cast<uint32_t>(QolSlotDataRequestState::kCancelled);
        const int error_code = g_fail_pending_during_unregister ? kQolErrInternal : kQolErrCancelled;
        settle(g_pending_request, failure(error_code, "write ended during unregister"), terminal_state);
        g_fail_pending_during_unregister = false;
    }
    g_registered = false;
    g_ready = false;
    g_handle = 0;
    *out_request = ++g_next_request;
    settle(*out_request, ok(), static_cast<uint32_t>(QolSlotDataRequestState::kCompleted));
    return ok();
}

QolResult fake_query(QolSlotDataRequest request, QolSlotDataStatus* out_status) {
    if (out_status == nullptr || out_status->size < sizeof(QolSlotDataStatus))
        return failure(kQolErrInvalidArgument, "invalid status");
    if (request == g_pending_request) {
        *out_status = g_last_status;
        return ok();
    }
    if (request != 0 && request == g_last_request_id && g_last_status.size != 0) {
        *out_status = g_last_status;
        return ok();
    }
    return failure(kQolErrNotFound, "request not found");
}

QolSlotDataApi fake_api() {
    QolSlotDataApi api{};
    api.abi = {sizeof(QolSlotDataApi), 1, 0};
    api.register_section = &fake_register;
    api.request_write = &fake_request_write;
    api.unregister = &fake_unregister;
    api.query = &fake_query;
    return api;
}

void reset_disk_and_metrics() {
    g_disk = {};
    g_write_count = 0;
    g_callback_count = 0;
    g_request_count = 0;
    g_fail_next_write = false;
    g_hold_next_write = false;
    g_pending_request = 0;
    g_pending_payload.clear();
    g_last_request_id = 0;
    g_submit_new_revision_in_request = false;
    g_fail_pending_during_unregister = false;
}

bool invoke_load(int slot, uint64_t epoch, uint32_t presence, uint16_t version,
                 const std::string& bytes) {
    g_session = {slot, static_cast<uint32_t>(QolSlotSessionState::kReady), epoch};
    g_ready = true;
    QolSlotDataContext ctx{g_session, static_cast<uint32_t>(QolSlotDataReason::kEnter), 0};
    ++g_callback_count;
    if (!succeeded(g_spec.session_ready(g_spec.user, &ctx))) return false;
    ++g_callback_count;
    return succeeded(g_spec.load(g_spec.user, &ctx, presence, version,
        presence == static_cast<uint32_t>(QolSlotDataPresence::kPresent)
            ? reinterpret_cast<const uint8_t*>(bytes.data()) : nullptr,
        presence == static_cast<uint32_t>(QolSlotDataPresence::kPresent) ? bytes.size() : 0));
}

autosell::Config sample(int rarity, bool enabled = true) {
    autosell::Config config;
    config.enabled = enabled;
    config.rarity = rarity;
    return config;
}

bool same(const autosell::Config& a, const autosell::Config& b) {
    return a.enabled == b.enabled && a.rarity == b.rarity && a.enhance == b.enhance &&
           a.socket == b.socket && a.gem_tier == b.gem_tier && a.gem_range == b.gem_range &&
           a.special_mask == b.special_mask;
}

void register_consumer(const QolSlotDataApi& api) {
    CHECK(autosell_store_register(1, &api));
}

void test_session_load_and_presence_errors(const QolSlotDataApi& api) {
    reset_disk_and_metrics();
    register_consumer(api);
    const std::string stored = autosell_config_to_json(sample(4));
    CHECK(invoke_load(0, 11, static_cast<uint32_t>(QolSlotDataPresence::kPresent), 2, stored));
    CHECK(same(autosell_get_runtime_config(), sample(4)));
    CHECK(autosell_scan_session_active(0));
    CHECK(!autosell_scan_save_enter(1));
    CHECK(!autosell_scan_session_active(0));
    CHECK(!autosell_store_session_matches(0));
    CHECK(invoke_load(0, 13, static_cast<uint32_t>(QolSlotDataPresence::kPresent), 2, stored));
    CHECK(autosell_scan_save_enter(0));
    CHECK(autosell_scan_session_active(0));

    CHECK(invoke_load(1, 12, static_cast<uint32_t>(QolSlotDataPresence::kMissing), 0, {}));
    CHECK(same(autosell_get_runtime_config(), autosell::Config{}));

    autosell_apply_config(sample(3));
    CHECK(!invoke_load(1, 12, static_cast<uint32_t>(QolSlotDataPresence::kUnreadable), 0, {}));
    CHECK(same(autosell_get_runtime_config(), sample(3)));
    CHECK(!invoke_load(1, 12, static_cast<uint32_t>(QolSlotDataPresence::kVersionUnsupported), 9, {}));
    CHECK(same(autosell_get_runtime_config(), sample(3)));
    CHECK(!invoke_load(1, 12, static_cast<uint32_t>(QolSlotDataPresence::kPresent), 2, "{bad"));
    CHECK(same(autosell_get_runtime_config(), sample(3)));
    CHECK(!autosell_scan_session_active(1));
    CHECK(!autosell_scan_save_enter(1));
    autosell_store_unregister();
}

void test_immediate_write_dedupe_and_retry(const QolSlotDataApi& api) {
    reset_disk_and_metrics();
    register_consumer(api);
    CHECK(invoke_load(0, 21, static_cast<uint32_t>(QolSlotDataPresence::kMissing), 0, {}));

    // Both production entry points call this common commit path.
    CHECK(autosell_store_commit_config(sample(2)));
    CHECK(g_write_count == 1);
    CHECK(g_disk[0].present && g_disk[0].version == 2);
    CHECK(g_disk[0].bytes.find("\"v\":2") != std::string::npos);
    CHECK(g_disk[0].bytes.find("\"rarity\":2") != std::string::npos);
    CHECK(autosell_store_commit_config(sample(2)));
    CHECK(g_write_count == 1);

    g_fail_next_write = true;
    CHECK(!autosell_store_commit_config(sample(1)));
    CHECK(g_write_count == 1);
    CHECK(autosell_store_commit_config(sample(1)));
    CHECK(g_write_count == 2);

    g_hold_next_write = true;
    CHECK(autosell_store_commit_config(sample(5)));
    const int accepted_count = g_write_count;
    const int accepted_requests = g_request_count;
    CHECK(autosell_store_commit_config(sample(5)));
    CHECK(g_write_count == accepted_count);
    CHECK(g_request_count == accepted_requests);
    CHECK(g_pending_request != 0);
    finish_write(g_pending_request);
    CHECK(g_write_count == accepted_count + 1);
    autosell_store_unregister();
}

void test_inflight_revision_completion_and_frame_retry(const QolSlotDataApi& api) {
    reset_disk_and_metrics();
    register_consumer(api);
    CHECK(invoke_load(0, 25, static_cast<uint32_t>(QolSlotDataPresence::kMissing), 0, {}));

    // The new edit lands before request_write returns its old request id.
    g_hold_next_write = true;
    g_submit_new_revision_in_request = true;
    g_new_revision = sample(3);
    CHECK(autosell_store_commit_config(sample(2)));
    CHECK(g_request_count == 1);
    CHECK(g_pending_request != 0);
    const auto old_request = g_pending_request;
    finish_write(old_request);
    CHECK(g_write_count == 2);
    CHECK(g_disk[0].bytes.find("\"rarity\":3") != std::string::npos);
    CHECK(g_request_count == 2);

    g_hold_next_write = true;
    CHECK(autosell_store_commit_config(sample(4)));
    const auto successful_old_request = g_pending_request;
    CHECK(autosell_store_commit_config(sample(5)));
    CHECK(g_request_count == 3);
    finish_write(successful_old_request);
    CHECK(g_write_count == 4);
    CHECK(g_request_count == 4);
    CHECK(g_disk[0].bytes.find("\"rarity\":5") != std::string::npos);

    // A later revision waits for the in-flight request; terminal failure is retried
    // by the scheduled flush after the bounded interval, not on every frame.
    g_hold_next_write = true;
    CHECK(autosell_store_commit_config(sample(6)));
    const auto failed_request = g_pending_request;
    CHECK(autosell_store_commit_config(sample(7)));
    CHECK(g_request_count == 5);
    run_retry_frame(0);
    fail_pending_write(failed_request);
    run_retry_frame(59);
    CHECK(g_request_count == 5);
    run_retry_frame(60);
    CHECK(g_request_count == 6);
    CHECK(g_write_count == 5);
    CHECK(g_disk[0].bytes.find("\"rarity\":7") != std::string::npos);
    autosell_store_unregister();
}

void test_unregistered_dirty_epoch_recovery(const QolSlotDataApi& api) {
    reset_disk_and_metrics();
    register_consumer(api);
    CHECK(invoke_load(0, 26, static_cast<uint32_t>(QolSlotDataPresence::kMissing), 0, {}));
    g_hold_next_write = true;
    CHECK(autosell_store_commit_config(sample(4)));
    const QolSlotDataContext stale_context = g_pending_context;
    const QolSlotDataRequest old_request = g_pending_request;
    CHECK(old_request != 0);
    const int callbacks_before_unregister = g_callback_count;

    autosell_store_unregister();
    CHECK(!g_registered);
    const int callbacks_at_terminal = g_callback_count;
    CHECK(callbacks_at_terminal == callbacks_before_unregister + 1);  // prepare_detach is terminal edge.
    CHECK(callbacks_at_terminal == g_callback_count);  // no callback after terminal.

    register_consumer(api);
    g_hold_next_write = true;
    CHECK(invoke_load(0, 27, static_cast<uint32_t>(QolSlotDataPresence::kMissing), 0, {}));
    const QolSlotDataRequest new_request = g_pending_request;
    CHECK(new_request != 0 && new_request != old_request);
    const int writes_before_stale_complete = g_write_count;
    g_spec.complete(g_spec.user, &stale_context,
                    static_cast<uint32_t>(QolSlotDataOutcome::kCommitted));
    CHECK(g_write_count == writes_before_stale_complete);

    run_retry_frame(1000);
    fail_pending_write(new_request);
    run_retry_frame(1059);
    CHECK(g_request_count == 2);
    run_retry_frame(1060);
    CHECK(g_request_count == 3);
    CHECK(g_disk[0].bytes.find("\"rarity\":4") != std::string::npos);
    autosell_store_unregister();
}

void test_pending_intent_slot_isolation_and_unregister(const QolSlotDataApi& api) {
    reset_disk_and_metrics();
    register_consumer(api);
    CHECK(invoke_load(0, 31, static_cast<uint32_t>(QolSlotDataPresence::kMissing), 0, {}));
    CHECK(autosell_store_commit_config(sample(3)));
    const std::string slot_zero = g_disk[0].bytes;

    g_hold_next_write = true;
    CHECK(autosell_store_commit_config(sample(2)));
    const auto deactivated_request = g_pending_request;
    CHECK(deactivated_request != 0);
    CHECK(g_disk[0].bytes == slot_zero);
    g_fail_pending_during_unregister = true;
    autosell_store_unregister();
    register_consumer(api);
    CHECK(invoke_load(1, 32, static_cast<uint32_t>(QolSlotDataPresence::kMissing), 0, {}));
    CHECK(same(autosell_get_runtime_config(), autosell::Config{}));
    CHECK(!g_disk[1].present);  // slot 0 的 dirty revision 不会串到 slot 1。
    autosell_store_unregister();
    register_consumer(api);
    CHECK(invoke_load(0, 33, static_cast<uint32_t>(QolSlotDataPresence::kMissing), 0, {}));
    CHECK(g_disk[0].bytes.find("\"rarity\":2") != std::string::npos);
    const std::string slot_zero_updated = g_disk[0].bytes;

    autosell_store_unregister();
    CHECK(!autosell_store_commit_config(sample(4)));
    CHECK(autosell_get_runtime_config().rarity == 4);
    g_disk[1].present = true;
    g_disk[1].version = 2;
    g_disk[1].bytes = autosell_config_to_json(sample(1));
    register_consumer(api);
    CHECK(!invoke_load(1, 34, static_cast<uint32_t>(QolSlotDataPresence::kUnreadable), 2, {}));
    CHECK(!autosell_store_session_matches(1));
    CHECK(!autosell_scan_session_active(1));
    CHECK(same(autosell_get_runtime_config(), sample(4)));
    CHECK(!invoke_load(1, 34, static_cast<uint32_t>(QolSlotDataPresence::kVersionUnsupported), 9, {}));
    CHECK(!autosell_scan_session_active(1));
    CHECK(same(autosell_get_runtime_config(), sample(4)));
    CHECK(!invoke_load(1, 34, static_cast<uint32_t>(QolSlotDataPresence::kPresent), 2, "{bad"));
    CHECK(!autosell_scan_session_active(1));
    CHECK(same(autosell_get_runtime_config(), sample(4)));
    autosell_store_unregister();
    register_consumer(api);
    CHECK(invoke_load(1, 35, static_cast<uint32_t>(QolSlotDataPresence::kPresent), 2,
                      autosell_config_to_json(sample(1))));
    CHECK(g_disk[1].present && g_disk[1].version == 2);
    CHECK(g_disk[1].bytes.find("\"rarity\":4") != std::string::npos);
    CHECK(g_disk[0].bytes == slot_zero_updated);

    const int callbacks_at_terminal = g_callback_count;
    autosell_store_unregister();
    CHECK(!g_registered);
    CHECK(g_callback_count == callbacks_at_terminal + 1);  // prepare_detach is the last callback.
    if (g_registered) {
        ++g_callback_count;
        (void)g_spec.session_ready(nullptr, nullptr);
    }
    CHECK(g_callback_count == callbacks_at_terminal + 1);
}

}  // namespace

namespace qol::features::autosell {
const QolHostSdk* host_sdk() { return &g_sdk; }
}  // namespace qol::features::autosell

void autosell_scan_sync_after_session_change() {}

void autosell_apply_config(const autosell::Config& config) {
    autosell_set_runtime_config(config);
}

int autosell_active_save_slot() {
    return autosell_scan_session_slot();
}

int main() {
    g_sdk.scheduler.add = &fake_scheduler_add;
    g_sdk.scheduler.remove = &fake_scheduler_remove;
    const QolSlotDataApi api = fake_api();
    test_session_load_and_presence_errors(api);
    test_immediate_write_dedupe_and_retry(api);
    test_inflight_revision_completion_and_frame_retry(api);
    test_unregistered_dirty_epoch_recovery(api);
    test_pending_intent_slot_isolation_and_unregister(api);
    std::printf("autosell_slot_data_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
