#include "plugins/autosell/config.h"
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
    if (g_pending_request == request) g_pending_request = 0;
}

void finish_write(QolSlotDataRequest request) {
    if (!g_registered || !g_ready || request == 0) return;
    QolSlotDataContext ctx{g_session, static_cast<uint32_t>(QolSlotDataReason::kRequested), 0};
    std::array<uint8_t, 4096> buffer{};
    uint64_t size = 0;
    ++g_callback_count;
    const QolResult captured = g_spec.capture(g_spec.user, &ctx, buffer.data(), buffer.size(), &size);
    if (!succeeded(captured) || size > buffer.size()) {
        settle(request, captured, static_cast<uint32_t>(QolSlotDataRequestState::kFailed));
        return;
    }
    auto& section = g_disk[static_cast<size_t>(g_session.slot)];
    section.present = true;
    section.version = g_spec.section_version;
    section.bytes.assign(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(size));
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
    const QolSlotDataRequest request = ++g_next_request;
    *out_request = request;
    if (g_hold_next_write) {
        g_hold_next_write = false;
        g_pending_request = request;
        g_last_status = {};
        g_last_status.size = sizeof(g_last_status);
        g_last_status.state = static_cast<uint32_t>(QolSlotDataRequestState::kPending);
        return ok();
    }
    finish_write(request);
    return ok();
}

QolResult fake_unregister(QolSlotDataHandle handle, QolSlotDataRequest* out_request) {
    if (handle != g_handle || out_request == nullptr || !g_registered)
        return failure(kQolErrNotFound, "handle not found");
    ++g_callback_count;
    const QolResult detached = g_spec.prepare_detach(g_spec.user, &g_session, 0);
    if (!succeeded(detached)) return detached;
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
    if (request != 0 && request == g_next_request && g_last_status.size != 0) {
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
    g_fail_next_write = false;
    g_hold_next_write = false;
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

    CHECK(invoke_load(1, 12, static_cast<uint32_t>(QolSlotDataPresence::kMissing), 0, {}));
    CHECK(same(autosell_get_runtime_config(), autosell::Config{}));

    autosell_apply_config(sample(3));
    CHECK(!invoke_load(1, 12, static_cast<uint32_t>(QolSlotDataPresence::kUnreadable), 0, {}));
    CHECK(same(autosell_get_runtime_config(), sample(3)));
    CHECK(!invoke_load(1, 12, static_cast<uint32_t>(QolSlotDataPresence::kVersionUnsupported), 9, {}));
    CHECK(same(autosell_get_runtime_config(), sample(3)));
    CHECK(!invoke_load(1, 12, static_cast<uint32_t>(QolSlotDataPresence::kPresent), 2, "{bad"));
    CHECK(same(autosell_get_runtime_config(), sample(3)));
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
    CHECK(autosell_store_commit_config(sample(5)));
    CHECK(g_write_count == accepted_count);
    CHECK(g_pending_request != 0);
    finish_write(g_pending_request);
    CHECK(g_write_count == accepted_count + 1);
    autosell_store_unregister();
}

void test_pending_intent_slot_isolation_and_unregister(const QolSlotDataApi& api) {
    reset_disk_and_metrics();
    register_consumer(api);
    CHECK(invoke_load(0, 31, static_cast<uint32_t>(QolSlotDataPresence::kMissing), 0, {}));
    CHECK(autosell_store_commit_config(sample(3)));
    const std::string slot_zero = g_disk[0].bytes;

    g_fail_next_write = true;
    CHECK(!autosell_store_commit_config(sample(2)));
    CHECK(g_disk[0].bytes == slot_zero);
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
    register_consumer(api);
    CHECK(invoke_load(1, 34, static_cast<uint32_t>(QolSlotDataPresence::kMissing), 0, {}));
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

void autosell_apply_config(const autosell::Config& config) {
    autosell_set_runtime_config(config);
}

int autosell_active_save_slot() {
    return g_ready ? g_session.slot : -1;
}

int main() {
    const QolSlotDataApi api = fake_api();
    test_session_load_and_presence_errors(api);
    test_immediate_write_dedupe_and_retry(api);
    test_pending_intent_slot_isolation_and_unregister(api);
    std::printf("autosell_slot_data_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
