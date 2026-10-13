#include "host/features/controller.h"
#include "host/features/receipt_formatter.h"
#include "host/owner_identity.h"
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <type_traits>

using namespace qol;
using namespace qol::contracts;
using namespace qol::host;
using namespace qol::features::core_settings;

namespace {
const JsonValue* json_member(const JsonValue& object, const char* key) {
    for (const auto& entry : object.object) if (entry.first == key) return &entry.second;
    return nullptr;
}

int business_calls = 0;
QolResult toggle(PluginContext&, uint8_t) { ++business_calls; return ok(); }
QolResult entry(PluginContext&) { ++business_calls; return ok(); }
QolResult endpoint_handler(const HttpReq*, HttpRes*, void*) { return ok(); }
const FeatureDefinition definitions[] = {
    {"toggle", 1, FeatureCategory::kHidden, FeatureType::kToggle, "metadata-name", "metadata-description", &toggle, nullptr},
    {"entry", 0, FeatureCategory::kTools, FeatureType::kEntry, "entry-name", "entry-description", nullptr, &entry}};
PluginDescriptor descriptor(const char* id) {
    PluginDescriptor value{};
    value.id = id;
    value.features = definitions;
    value.feature_count = 2;
    return value;
}
QolOwnerToken owner(void* identity, const char* id) {
    assert(owner_register(identity, id));
    QolOwnerToken token = 0;
    uint64_t generation = 0;
    assert(succeeded(owner_identity_api().query(identity, &token, &generation)));
    return token;
}
std::string bytes(const std::string& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
}
FeatureReceipt await(FeatureController& controller, FeatureReceipt receipt) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        assert(succeeded(controller.receipt(receipt.request_id, &receipt)));
        if (receipt.persistence != FeaturePersistence::kPending) return receipt;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    std::fprintf(stderr, "worker receipt timeout\n");
    std::abort();
}
struct Pending {
    std::mutex mutex;
    std::condition_variable changed;
    bool block = false;
    bool entered = false;
    bool release = false;
    bool throw_next = false;
    bool reenter = false;
    CoreSettingsConfigService* service = nullptr;
    CoreSettingsConfigCommit nested;
    std::vector<uint64_t> revisions;
    std::vector<PluginDesiredSnapshot> latest;
    std::thread::id callback_thread;
};
void pending(void* context, uint64_t revision, const std::vector<PluginDesiredSnapshot>& snapshots) {
    auto& state = *static_cast<Pending*>(context);
    std::unique_lock<std::mutex> lock(state.mutex);
    state.revisions.push_back(revision);
    state.latest = snapshots;
    state.callback_thread = std::this_thread::get_id();
    if (state.block) {
        state.entered = true;
        state.changed.notify_all();
        assert(state.changed.wait_for(lock, std::chrono::seconds(4), [&] { return state.release; }));
        state.block = false;
    }
    if (state.reenter) {
        state.reenter = false;
        lock.unlock();
        auto nested = state.service->commit_feature_model("plugin.alpha", FeatureConfigField::kFeature, "entry", "true");
        lock.lock();
        state.nested = nested;
    }
    if (state.throw_next) {
        state.throw_next = false;
        throw std::runtime_error("injected pending observer failure");
    }
}
QolResult entry_admission(void* context, const char*, const char*) {
    ++*static_cast<int*>(context);
    return ok();
}

struct ReceiptNotice {
    std::mutex mutex;
    std::condition_variable changed;
    uint32_t calls = 0;
    bool completed = false;
    std::thread::id callback_thread{};
    FeatureReceipt receipt{};
};

void record_receipt(void* raw, const FeatureReceipt& receipt) noexcept {
    auto& notice = *static_cast<ReceiptNotice*>(raw);
    {
        std::lock_guard<std::mutex> lock(notice.mutex);
        ++notice.calls;
        notice.completed = true;
        notice.callback_thread = std::this_thread::get_id();
        notice.receipt = receipt;
    }
    notice.changed.notify_all();
}

struct ReentrantReceiptNotice {
    std::mutex mutex;
    std::condition_variable changed;
    FeatureController* controller = nullptr;
    uint64_t subscription_id = 0;
    uint32_t calls = 0;
    bool completed = false;
    QolResult read_status{};
    QolResult child_admission{};
    QolResult self_unsubscribe{};
    FeatureReceipt delivered{};
    FeatureReceipt read_back{};
    FeatureReceipt child{};
};

void reentrant_receipt(void* raw, const FeatureReceipt& receipt) noexcept {
    auto& notice = *static_cast<ReentrantReceiptNotice*>(raw);
    FeatureReceipt read_back{};
    const QolResult read_status = notice.controller->receipt(receipt.request_id, &read_back);
    static constexpr char kChildJson[] = "\"created by receipt callback\"";
    FeatureReceipt child{};
    const QolResult child_admission = notice.controller->request_set_config(
        "plugin.alpha", "receipt_child", kChildJson, sizeof(kChildJson) - 1, &child);
    const QolResult self_unsubscribe =
        notice.controller->unsubscribe_receipt(notice.subscription_id);
    {
        std::lock_guard<std::mutex> lock(notice.mutex);
        ++notice.calls;
        notice.completed = true;
        notice.delivered = receipt;
        notice.read_status = read_status;
        notice.read_back = read_back;
        notice.child_admission = child_admission;
        notice.child = child;
        notice.self_unsubscribe = self_unsubscribe;
    }
    notice.changed.notify_all();
}

struct BlockingReceiptNotice {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    uint32_t calls = 0;
    FeatureReceipt receipt{};
};

void blocking_receipt(void* raw, const FeatureReceipt& receipt) noexcept {
    auto& notice = *static_cast<BlockingReceiptNotice*>(raw);
    std::unique_lock<std::mutex> lock(notice.mutex);
    ++notice.calls;
    notice.receipt = receipt;
    notice.entered = true;
    notice.changed.notify_all();
    assert(notice.changed.wait_for(lock, std::chrono::seconds(4),
                                   [&] { return notice.release; }));
}

void block_next_pending(Pending& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.block = true;
    state.entered = false;
    state.release = false;
}

void wait_pending_entered(Pending& state) {
    std::unique_lock<std::mutex> lock(state.mutex);
    assert(state.changed.wait_for(lock, std::chrono::seconds(4), [&] { return state.entered; }));
}

void release_pending(Pending& state) {
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.release = true;
    }
    state.changed.notify_all();
}

void test_receipt_push(const FeatureDirectory& directory, const std::string& root) {
    const std::string path = root + "/receipt-push";
    assert(::mkdir(path.c_str(), 0700) == 0);
    CoreSettingsConfigService service;
    FeatureController controller(service);
    Pending state;
    state.service = &service;
    assert(controller.initialize(directory, path, &pending, &state).ok());

    // Hold the worker after the durable write so the request can be subscribed while pending.
    block_next_pending(state);
    FeatureReceipt parent{};
    static constexpr char kParentJson[] = "\"parent\"";
    assert(succeeded(controller.request_set_config("plugin.alpha", "receipt_parent",
                                                   kParentJson, sizeof(kParentJson) - 1,
                                                   &parent)));
    assert(parent.request_id != 0 && parent.persistence == FeaturePersistence::kPending);
    wait_pending_entered(state);

    ReentrantReceiptNotice reentrant;
    reentrant.controller = &controller;
    assert(succeeded(controller.subscribe_receipt(parent.request_id, &reentrant_receipt,
                                                   &reentrant, &reentrant.subscription_id)));
    assert(reentrant.subscription_id != 0);
    release_pending(state);
    {
        std::unique_lock<std::mutex> lock(reentrant.mutex);
        assert(reentrant.changed.wait_for(lock, std::chrono::seconds(4),
                                          [&] { return reentrant.completed; }));
        assert(reentrant.calls == 1);
        assert(succeeded(reentrant.read_status));
        assert(reentrant.read_back.persistence == FeaturePersistence::kPersisted);
        assert(succeeded(reentrant.child_admission) && reentrant.child.request_id != 0);
        assert(succeeded(reentrant.self_unsubscribe));
    }
    assert(await(controller, reentrant.child).persistence == FeaturePersistence::kPersisted);
    FeatureReceipt stored_parent{};
    assert(succeeded(controller.receipt(parent.request_id, &stored_parent)));
    assert(feature_receipt_json(stored_parent) == feature_receipt_json(reentrant.delivered));
    assert(controller.unsubscribe_receipt(reentrant.subscription_id).code == kQolErrNotFound);

    // A late subscription synchronously replays one terminal snapshot with no live token.
    ReceiptNotice late;
    uint64_t late_subscription = 99;
    const auto caller_thread = std::this_thread::get_id();
    assert(succeeded(controller.subscribe_receipt(parent.request_id, &record_receipt, &late,
                                                  &late_subscription)));
    assert(late_subscription == 0 && late.calls == 1 && late.completed);
    assert(late.callback_thread == caller_thread &&
           late.receipt.persistence == FeaturePersistence::kPersisted);

    // Unknown/rejected requests never acquire an observer or invoke a callback.
    ReceiptNotice unknown;
    uint64_t unknown_subscription = 77;
    assert(controller.subscribe_receipt(999999, &record_receipt, &unknown,
                                        &unknown_subscription).code == kQolErrNotFound);
    assert(unknown_subscription == 0 && unknown.calls == 0);

    // CAS conflict is a terminal failed receipt and notifies the pending subscriber unchanged.
    const uint64_t expected_revision = service.revision();
    block_next_pending(state);
    FeatureReceipt first_cas{};
    FeatureReceipt conflict_cas{};
    static constexpr char kCasOne[] = "1";
    static constexpr char kCasTwo[] = "2";
    assert(succeeded(controller.request_set_config_if_revision(
        "plugin.alpha", "receipt_cas", kCasOne, sizeof(kCasOne) - 1, expected_revision,
        &first_cas)));
    wait_pending_entered(state);
    assert(succeeded(controller.request_set_config_if_revision(
        "plugin.alpha", "receipt_cas", kCasTwo, sizeof(kCasTwo) - 1, expected_revision,
        &conflict_cas)));
    ReceiptNotice conflict_notice;
    uint64_t conflict_subscription = 0;
    assert(succeeded(controller.subscribe_receipt(conflict_cas.request_id, &record_receipt,
                                                  &conflict_notice, &conflict_subscription)));
    assert(conflict_subscription != 0);
    release_pending(state);
    assert(await(controller, first_cas).persistence == FeaturePersistence::kPersisted);
    {
        std::unique_lock<std::mutex> lock(conflict_notice.mutex);
        assert(conflict_notice.changed.wait_for(lock, std::chrono::seconds(4),
                                                [&] { return conflict_notice.completed; }));
        assert(conflict_notice.calls == 1);
        assert(conflict_notice.receipt.persistence == FeaturePersistence::kFailed);
        assert(conflict_notice.receipt.error_code == kQolErrConflict);
    }
    FeatureReceipt stored_conflict{};
    assert(succeeded(controller.receipt(conflict_cas.request_id, &stored_conflict)));
    assert(feature_receipt_json(stored_conflict) == feature_receipt_json(conflict_notice.receipt));

    // External unsubscribe waits until the callback has returned and its context is safe to free.
    block_next_pending(state);
    FeatureReceipt blocked_request{};
    static constexpr char kBlockedJson[] = "true";
    assert(succeeded(controller.request_set_config("plugin.alpha", "receipt_blocked",
                                                   kBlockedJson, sizeof(kBlockedJson) - 1,
                                                   &blocked_request)));
    wait_pending_entered(state);
    BlockingReceiptNotice blocking_notice;
    uint64_t blocking_subscription = 0;
    assert(succeeded(controller.subscribe_receipt(blocked_request.request_id, &blocking_receipt,
                                                  &blocking_notice, &blocking_subscription)));
    release_pending(state);
    {
        std::unique_lock<std::mutex> lock(blocking_notice.mutex);
        assert(blocking_notice.changed.wait_for(lock, std::chrono::seconds(4),
                                                [&] { return blocking_notice.entered; }));
    }
    std::atomic<bool> unsubscribe_done{false};
    QolResult unsubscribe_result{};
    std::thread unsubscriber([&] {
        unsubscribe_result = controller.unsubscribe_receipt(blocking_subscription);
        unsubscribe_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!unsubscribe_done.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(blocking_notice.mutex);
        blocking_notice.release = true;
    }
    blocking_notice.changed.notify_all();
    unsubscriber.join();
    assert(unsubscribe_done.load(std::memory_order_acquire) &&
           succeeded(unsubscribe_result) && blocking_notice.calls == 1);

    // Keep the worker occupied after one request, fill its bounded queue, and verify a
    // rejected admission cannot acquire a request observer.
    block_next_pending(state);
    FeatureReceipt queue_blocker{};
    assert(succeeded(controller.request_set_config("plugin.alpha", "queue_blocker",
                                                   kBlockedJson, sizeof(kBlockedJson) - 1,
                                                   &queue_blocker)));
    wait_pending_entered(state);
    for (uint32_t index = 0; index < 256; ++index) {
        const std::string value = std::to_string(index);
        FeatureReceipt queued{};
        assert(succeeded(controller.request_set_config("plugin.alpha", "queue_fill",
                                                        value.data(),
                                                        static_cast<uint32_t>(value.size()),
                                                        &queued)));
        assert(queued.request_id != 0);
    }
    FeatureReceipt rejected{};
    assert(controller.request_set_config("plugin.alpha", "queue_overflow", kBlockedJson,
                                         sizeof(kBlockedJson) - 1, &rejected).code == kQolErrBusy);
    assert(rejected.request_id == 0 && rejected.error_code == kQolErrBusy);
    ReceiptNotice rejected_notice;
    uint64_t rejected_subscription = 55;
    assert(controller.subscribe_receipt(rejected.request_id, &record_receipt, &rejected_notice,
                                        &rejected_subscription).code == kQolErrInvalidArgument);
    assert(rejected_subscription == 0 && rejected_notice.calls == 0);
    release_pending(state);
    controller.shutdown();
}

void test_seed_type_validation(const std::string& root, const FeatureDirectory& directory) {
    struct InvalidSchema {
        const char* name;
        const char* object;
        const char* message;
    };
    const InvalidSchema cases[] = {
        {"enable", R"({"enable":null})", "plugin enable must be boolean"},
        {"feature", R"({"features":{"toggle":1}})", "registered feature must be boolean"},
        {"features-section", R"({"features":false})", "plugin features must be an object"},
        {"config-section", R"({"config":[]})", "plugin config must be an object"}};
    for (const auto& test : cases) {
        const auto path = root + "/invalid-" + test.name;
        assert(::mkdir(path.c_str(), 0700) == 0);
        // The earlier plugin needs defaults; a later invalid plugin must reject the whole batch.
        const std::string original = std::string("{\"plugin.alpha\":{},\"plugin.beta\":") + test.object + "}";
        {
            std::ofstream file(path + "/config.json");
            file << original;
        }
        Pending state;
        CoreSettingsConfigService service;
        state.service = &service;
        assert(service.prepare_features(directory.definitions(), &pending, &state).ok());
        assert(service.initialize(path).ok());
        const auto revision = service.revision();
        assert(revision == 1 && state.revisions.empty());
        JsonValue before;
        const auto before_read = service.read_namespace("plugin.alpha", &before);
        assert(!before_read.ok());
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto seeded = service.seed_features();
            assert(seeded.result.error == ConfigStoreError::kValidation);
            assert(seeded.result.message == test.message && !seeded.result.persisted);
            assert(!seeded.changed && !seeded.memory_committed && !seeded.durable_commit);
            assert(bytes(path + "/config.json") == original);
            assert(service.revision() == revision && state.revisions.empty());
            JsonValue after;
            assert(service.read_namespace("plugin.alpha", &after).error == before_read.error);
            assert(serialize_json_value(after) == serialize_json_value(before));
        }
        // The Store registration set also stays unchanged; repeated rejection proves that
        // the invalid persisted values were not replaced by defaults in memory.
        ConfigStore store;
        assert(store.prepare_features(directory.definitions()).ok());
        assert(store.init(path).ok());
        const auto ids = store.registered_plugin_ids();
        assert(ids.empty());
        assert(store.seed_features().result.error == ConfigStoreError::kValidation);
        assert(store.registered_plugin_ids() == ids);
        assert(store.seed_features().result.error == ConfigStoreError::kValidation);
        assert(bytes(path + "/config.json") == original);
    }

    const auto valid_path = root + "/valid-existing-bools";
    assert(::mkdir(valid_path.c_str(), 0700) == 0);
    {
        std::ofstream file(valid_path + "/config.json");
        file << R"({"plugin.beta":{"enable":false,"features":{"toggle":false,"entry":true},"config":{}}})";
    }
    CoreSettingsConfigService service;
    assert(service.prepare_features(directory.definitions(), nullptr, nullptr).ok());
    assert(service.initialize(valid_path).ok());
    assert(service.seed_features().result.ok());
    std::vector<PluginDesiredSnapshot> snapshots;
    assert(service.desired_snapshots(&snapshots).ok());
    assert(snapshots[0].desired_enable && snapshots[0].any_feature);
    assert(!snapshots[1].desired_enable && snapshots[1].any_feature);
    JsonValue existing;
    assert(service.read_feature_namespace("plugin.beta", &existing).ok());
    const auto json = serialize_json_value(existing);
    assert(json.find("\"toggle\":false") != std::string::npos);
    assert(json.find("\"entry\":true") != std::string::npos);
    std::puts("host_feature_seed_validation: pass (enable/feature/features-section/config-section rejected without file/memory/revision/effect changes; existing bools preserved)");
}

struct SnapshotReentry {
    CoreSettingsConfigService* service = nullptr;
    bool armed = false;
    std::vector<FeatureControlSnapshot> events;
};
void capture_snapshot(void* raw, const FeatureControlSnapshot& snapshot) {
    static_cast<SnapshotReentry*>(raw)->events.push_back(snapshot);
}
void reenter_two_revisions(void* raw, uint64_t, const std::vector<PluginDesiredSnapshot>&) {
    auto& state = *static_cast<SnapshotReentry*>(raw);
    if (!state.armed) return;
    state.armed = false;
    const auto on = state.service->commit_feature_model("plugin.alpha", FeatureConfigField::kFeature, "toggle", "true");
    const auto off = state.service->commit_feature_model("plugin.alpha", FeatureConfigField::kFeature, "toggle", "false");
    assert(on.committed && off.committed && off.revision == on.revision + 1);
    assert(on.apply_state == SettingsConfigApplyState::kPendingFrame);
    assert(off.apply_state == SettingsConfigApplyState::kPendingFrame);
}
void test_immutable_reentry(const std::string& root) {
    const auto path = root + "/immutable-reentry";
    assert(::mkdir(path.c_str(), 0700) == 0);
    {
        std::ofstream file(path + "/config.json");
        file << R"({"plugin.alpha":{"enable":true,"features":{"toggle":false,"entry":false},"config":{"mark":0}}})";
    }
    FeatureDirectory directory;
    assert(directory.add(descriptor("plugin.alpha")).ok());
    SnapshotReentry state;
    CoreSettingsConfigService service;
    state.service = &service;
    FeatureController controller(service);
    controller.bind_state_observer(&capture_snapshot, &state);
    assert(controller.initialize(directory, path, &reenter_two_revisions, &state).ok());
    state.armed = true;
    FeatureReceipt receipt{};
    assert(succeeded(controller.request_set_config("plugin.alpha", "mark", "\"R\"", 3, &receipt)));
    receipt = await(controller, receipt);
    controller.shutdown();
    assert(state.events.size() == 4); // bootstrap, R, reentrant R+1(on), R+2(off)
    for (size_t index = 1; index < state.events.size(); ++index) {
        const auto& event = state.events[index];
        assert(event.revision == receipt.revision + index - 1);
        const bool expected = index == 2;
        assert(event.desired.size() == 1 && event.desired[0].desired_enable == (index != 3));
        assert(event.desired[0].any_feature == expected);
        const auto json = serialize_json_value(event.namespaces[0].second);
        assert(event.namespaces[0].first == "plugin.alpha");
        assert(json.find(expected ? "\"toggle\":true" : "\"toggle\":false") != std::string::npos);
        assert(json.find("\"entry\":false") != std::string::npos);
        assert(json.find("\"mark\":\"R\"") != std::string::npos);
    }
    std::puts("immutable_feature_events: pass (reentrant R/R+1-on/R+2-off revision, desired, features and config are consistent)");
}

// read_registered is the /api/config/list serialization source. It must project the on-disk
// schema key order (enable, features, config), keep per-value types, and escape JSON strings.
void test_registered_projection(const std::string& root) {
    const auto path = root + "/registered-projection";
    assert(::mkdir(path.c_str(), 0700) == 0);
    {
        std::ofstream file(path + "/config.json");
        // The text value carries a quote, a backslash, control characters (newline/tab) and a
        // non-ASCII codepoint (\u00e9); the projection must re-escape it canonically.
        file << R"({"plugin.alpha":{"enable":false,"features":{"toggle":true,"entry":false},"config":{"text":"a\"b\\c\nd\te caf\u00e9","list":[1,true,null]}}})";
    }
    FeatureDirectory directory;
    assert(directory.add(descriptor("plugin.alpha")).ok());
    CoreSettingsConfigService service;
    FeatureController controller(service);
    assert(controller.initialize(directory, path, nullptr, nullptr).ok());
    char buffer[8192]{};
    uint32_t size = 0;
    assert(succeeded(controller.read_registered(buffer, sizeof(buffer), &size)));
    const std::string json(buffer, size);
    // Projection key order matches the on-disk schema: enable, features, config.
    assert(json.rfind(R"({"plugin.alpha":{"enable":false,"features":{"toggle":true,"entry":false},"config":{)", 0) == 0);
    // Types: booleans for enable/features, raw JSON for config values.
    assert(json.find(R"("toggle":true)") != std::string::npos);
    assert(json.find(R"("entry":false)") != std::string::npos);
    assert(json.find(R"("list":[1,true,null])") != std::string::npos);
    // Escaping: quotes/backslashes escaped, control chars as \n/\t, non-ASCII preserved as UTF-8.
    assert(json.find("\"text\":\"a\\\"b\\\\c\\nd\\te caf") != std::string::npos);
    assert(json.find("\xc3\xa9\"") != std::string::npos);
    controller.shutdown();
    std::puts("registered_projection: pass (enable,features,config order; boolean/array types; quote/backslash/control/non-ASCII escaping)");
}

void test_feature_writes_derive_enable_atomically(const std::string& root) {
    const auto path = root + "/feature-derived-enable";
    assert(::mkdir(path.c_str(), 0700) == 0);
    {
        std::ofstream file(path + "/config.json");
        // Bootstrap and explicit desired writes retain their own value; only a feature
        // submission derives enable from the registered post-edit feature set.
        file << R"({"plugin.alpha":{"enable":true,"features":{"toggle":false,"entry":false},"config":{}}})";
    }

    FeatureDirectory directory;
    assert(directory.add(descriptor("plugin.alpha")).ok());
    CoreSettingsConfigService service;
    FeatureController controller(service);
    Pending state;
    state.service = &service;
    assert(controller.initialize(directory, path, &pending, &state).ok());

    auto expect_desired = [&](bool enabled, bool any_feature) {
        std::vector<PluginDesiredSnapshot> snapshots;
        assert(controller.desired(&snapshots).ok());
        assert(snapshots.size() == 1);
        assert(snapshots[0].desired_enable == enabled);
        assert(snapshots[0].any_feature == any_feature);
    };
    auto submit_feature = [&](const char* key, uint8_t value) {
        FeatureReceipt receipt{};
        assert(succeeded(controller.request_set_feature("plugin.alpha", key, value, &receipt)));
        receipt = await(controller, receipt);
        assert(receipt.persistence == FeaturePersistence::kPersisted);
        return receipt;
    };
    auto submit_desired = [&](uint8_t value) {
        FeatureReceipt receipt{};
        assert(succeeded(controller.request_desired("plugin.alpha", value, &receipt)));
        receipt = await(controller, receipt);
        assert(receipt.persistence == FeaturePersistence::kPersisted);
    };

    expect_desired(true, false); // Startup does not normalize persisted desired state.
    const auto initial_revision = service.revision();
    assert(controller.invoke_entry("plugin.alpha", "entry").code == kQolErrBlocked);
    expect_desired(true, false); // A rejected entry invocation changes neither feature nor enable.
    assert(service.revision() == initial_revision);

    submit_desired(0);
    expect_desired(false, false);
    submit_feature("toggle", 1); // A feature can turn on while enable was false.
    expect_desired(true, true);
    submit_feature("entry", 1); // Entry booleans participate in the aggregate.
    expect_desired(true, true);
    submit_feature("toggle", 0);
    expect_desired(true, true); // One remaining enabled feature keeps enable true.
    submit_feature("entry", 0);
    expect_desired(false, false); // Turning the last feature off derives enable=false.

    submit_desired(1);
    expect_desired(true, false);
    submit_feature("entry", 0); // Repeated false repairs a stale true desired value.
    expect_desired(false, false);
    submit_feature("toggle", 1);
    expect_desired(true, true);
    submit_desired(0);
    expect_desired(false, true);
    submit_feature("toggle", 1); // Repeated true repairs a stale false desired value.
    expect_desired(true, true);

    submit_feature("entry", 1);
    submit_desired(0);
    int entry_requests = 0;
    controller.bind_entry_admission(&entry_admission, &entry_requests);
    const auto before_invocation_revision = service.revision();
    assert(succeeded(controller.invoke_entry("plugin.alpha", "entry")));
    assert(entry_requests == 1 && business_calls == 0);
    assert(service.revision() == before_invocation_revision);
    expect_desired(false, true); // Invoking an entry does not derive or repair enable.

    submit_feature("entry", 0); // Keep toggle on so the following write would turn all off.
    expect_desired(true, true);
    const uint64_t cas_revision = service.revision();
    size_t cas_effects_before = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        cas_effects_before = state.revisions.size();
    }
    FeatureReceipt cas_first{}, cas_stale{};
    assert(succeeded(controller.request_set_config_if_revision(
        "plugin.alpha", "cas", "1", 1, cas_revision, &cas_first)));
    assert(succeeded(controller.request_set_config_if_revision(
        "plugin.alpha", "cas", "2", 1, cas_revision, &cas_stale)));
    cas_first = await(controller, cas_first);
    cas_stale = await(controller, cas_stale);
    assert(cas_first.persistence == FeaturePersistence::kPersisted &&
           cas_first.revision == cas_revision + 1);
    assert(cas_stale.persistence == FeaturePersistence::kFailed &&
           cas_stale.error_code == kQolErrConflict &&
           cas_stale.actual == FeatureActual::kNotSubmitted &&
           cas_stale.revision == cas_first.revision);
    assert(service.revision() == cas_first.revision);
    char cas_value[32]{};
    uint32_t cas_size = 0;
    assert(succeeded(controller.read_config("plugin.alpha", "cas", cas_value,
                                             sizeof(cas_value), &cas_size)));
    assert(std::string(cas_value, cas_size) == "1");
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        assert(state.revisions.size() == cas_effects_before + 1);
    }

    const std::string durable_before_failure = bytes(path + "/config.json");
    const auto revision_before_failure = service.revision();
    size_t effects_before_failure;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        effects_before_failure = state.revisions.size();
    }
    assert(::rename((path + "/config.json").c_str(), (path + "/saved.json").c_str()) == 0);
    assert(::mkdir((path + "/config.json").c_str(), 0700) == 0);
    FeatureReceipt failed{};
    assert(succeeded(controller.request_set_feature("plugin.alpha", "toggle", 0, &failed)));
    failed = await(controller, failed);
    assert(failed.persistence == FeaturePersistence::kFailed &&
           failed.actual == FeatureActual::kNotSubmitted);
    assert(service.revision() == revision_before_failure);
    assert(bytes(path + "/saved.json") == durable_before_failure);
    uint8_t toggle_value = 0, entry_value = 1;
    assert(succeeded(controller.read_feature("plugin.alpha", "toggle", &toggle_value)) && toggle_value);
    assert(succeeded(controller.read_feature("plugin.alpha", "entry", &entry_value)) && !entry_value);
    expect_desired(true, true); // Feature and derived enable did not partially commit.
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        assert(state.revisions.size() == effects_before_failure);
    }
    assert(business_calls == 0);
    controller.shutdown();
    std::puts("feature_write_derived_enable: pass (multi-feature/entry aggregation, last-off, on-from-disabled, repeated same-value repair, entry invocation guard, atomic persistence failure)");
}

int config_validator_calls = 0;
QolResult validate_api_config(const char* key, const char* json, uint32_t size) {
    ++config_validator_calls;
    if (key == nullptr || (size != 0 && json == nullptr))
        return error(kQolErrInvalidArgument, nullptr, 0);
    if (std::strcmp(key, "reject") == 0)
        return error(kQolErrInvalidArgument, "rejected by descriptor", 23);
    if (std::strcmp(key, "listen_address") != 0 && std::strcmp(key, "listen_port") != 0)
        return ok();
    JsonValue value;
    if (!parse_json_document(std::string(json == nullptr ? "" : json, size), &value).ok())
        return error(kQolErrInvalidArgument, nullptr, 0);
    if (std::strcmp(key, "listen_address") == 0)
        return value.kind == JsonValue::Kind::kString && !value.scalar.empty()
            ? ok() : error(kQolErrInvalidArgument, nullptr, 0);
    if (value.kind != JsonValue::Kind::kNumber) return error(kQolErrInvalidArgument, nullptr, 0);
    try {
        std::size_t used = 0;
        const long port = std::stol(value.scalar, &used);
        return used == value.scalar.size() && port >= 1 && port <= 65535
            ? ok() : error(kQolErrInvalidArgument, nullptr, 0);
    } catch (const std::exception&) {
        return error(kQolErrInvalidArgument, nullptr, 0);
    }
}

const FeatureDefinition migration_api_features[] = {
    {"api.enabled", 1, FeatureCategory::kGeneral, FeatureType::kToggle,
     "API", "", &toggle, nullptr},
};
const FeatureDefinition migration_platform_features[] = {
    {"debug.logging", 0, FeatureCategory::kGeneral, FeatureType::kToggle,
     "Debug", "Debug logging", &toggle, nullptr},
};
const FeatureDefinition migration_security_features[] = {
    {"op.enabled", 0, FeatureCategory::kGeneral, FeatureType::kToggle,
     "OP", "Operation permission", &toggle, nullptr},
};

PluginDescriptor migration_descriptor(const char* id, const FeatureDefinition* features,
                                      uint32_t count) {
    PluginDescriptor value{};
    value.id = id;
    value.version = "1.0.0";
    value.features = features;
    value.feature_count = count;
    return value;
}

FeatureDirectory migration_directory() {
    FeatureDirectory directory;
    auto api = migration_descriptor("core.api", migration_api_features, 1);
    api.validate_config = &validate_api_config;
    assert(directory.add(api).ok());
    assert(directory.add_host_owner(migration_descriptor(
        "module.platform", migration_platform_features, 1)).ok());
    assert(directory.add_host_owner(migration_descriptor(
        "module.security", migration_security_features, 1)).ok());
    return directory;
}

void write_migration_file(const std::string& path, bool platform_api, bool registry_api,
                          bool old_op, bool target_api, bool target_op,
                          bool include_api_target, bool include_op_target,
                          int target_port = 0) {
    assert(::mkdir(path.c_str(), 0700) == 0);
    std::ofstream file(path + "/config.json");
    file << "{\"module.platform\":{\"enable\":true,\"features\":{\"debug.logging\":false,\"api.enabled\":"
         << (platform_api ? "true" : "false") << ",\"op.enabled\":"
         << (old_op ? "true" : "false") << "},\"config\":{\"listen_address\":\"127.0.0.1\",\"listen_port\":9000}},"
         << "\"core.api\":{\"enable\":true,\"features\":{\"core.api.http_registry\":"
         << (registry_api ? "true" : "false");
    if (include_api_target) file << ",\"api.enabled\":" << (target_api ? "true" : "false");
    file << "},\"config\":{";
    if (target_port != 0) file << "\"listen_port\":" << target_port;
    file << "}},\"module.security\":{\"enable\":true,\"features\":{";
    if (include_op_target) file << "\"op.enabled\":" << (target_op ? "true" : "false");
    file << "}}}";
    file.close();
    assert(file.good());
}

void test_host_owner_migration(const std::string& root) {
    const bool gates[] = {false, true};
    uint32_t case_number = 0;
    for (bool platform_api : gates) for (bool registry_api : gates) for (bool old_op : gates) {
        const auto path = root + "/owner-migration-" + std::to_string(case_number++);
        write_migration_file(path, platform_api, registry_api, old_op, false, false,
                             false, false);
        auto directory = migration_directory();
        CoreSettingsConfigService service;
        config_validator_calls = 0;
        assert(service.prepare_features(directory.definitions(), nullptr, nullptr).ok());
        assert(service.initialize(path).ok());
        const auto seeded = service.seed_features();
        assert(seeded.result.ok() && seeded.durable_commit && seeded.memory_committed);
        JsonValue api_namespace, platform_namespace, security_namespace;
        assert(service.read_feature_namespace("core.api", &api_namespace).ok());
        assert(service.read_feature_namespace("module.platform", &platform_namespace).ok());
        assert(service.read_feature_namespace("module.security", &security_namespace).ok());
        const JsonValue* api_features = json_member(api_namespace, "features");
        const JsonValue* security_features = json_member(security_namespace, "features");
        assert(api_features != nullptr && security_features != nullptr);
        const JsonValue* api_value = json_member(*api_features, "api.enabled");
        const JsonValue* op_value = json_member(*security_features, "op.enabled");
        assert(api_value && api_value->boolean == (platform_api && registry_api));
        assert(op_value && op_value->boolean == old_op);
        const JsonValue* platform_features = json_member(platform_namespace, "features");
        const JsonValue* platform_config = json_member(platform_namespace, "config");
        assert(platform_features && json_member(*platform_features, "api.enabled") == nullptr &&
               json_member(*platform_features, "op.enabled") == nullptr);
        assert(platform_config && json_member(*platform_config, "listen_address") == nullptr &&
               json_member(*platform_config, "listen_port") == nullptr);
        const JsonValue* api_config = json_member(api_namespace, "config");
        assert(api_config && json_member(*api_config, "listen_address") &&
               json_member(*api_config, "listen_port"));
        assert(config_validator_calls >= 2);
        const std::string persisted = bytes(path + "/config.json");
        assert(persisted.find("core.api.http_registry") == std::string::npos);
        assert(persisted.find("\"api.enabled\"") != std::string::npos);
        assert(persisted.find("\"op.enabled\"") != std::string::npos);

        const auto revision = service.revision();
        const auto rejected = service.commit_feature_model("core.api", FeatureConfigField::kConfig,
                                                            "reject", "false");
        assert(!rejected.committed && rejected.result.error == ConfigStoreError::kValidation &&
               service.revision() == revision);
    }

    const auto precedence = root + "/owner-migration-new-target-wins";
    write_migration_file(precedence, false, false, false, true, true, true, true, 8080);
    auto directory = migration_directory();
    CoreSettingsConfigService service;
    assert(service.prepare_features(directory.definitions(), nullptr, nullptr).ok());
    assert(service.initialize(precedence).ok());
    assert(service.seed_features().result.ok());
    JsonValue api_namespace, security_namespace;
    assert(service.read_feature_namespace("core.api", &api_namespace).ok());
    assert(service.read_feature_namespace("module.security", &security_namespace).ok());
    const JsonValue* api_features = json_member(api_namespace, "features");
    const JsonValue* security_features = json_member(security_namespace, "features");
    assert(json_member(*api_features, "api.enabled")->boolean);
    assert(json_member(*security_features, "op.enabled")->boolean);
    const JsonValue* api_config = json_member(api_namespace, "config");
    assert(json_member(*api_config, "listen_port")->scalar == "8080");

    const auto target_false = root + "/owner-migration-new-false-wins";
    write_migration_file(target_false, true, true, true, false, false, true, true);
    auto false_directory = migration_directory();
    CoreSettingsConfigService false_service;
    assert(false_service.prepare_features(false_directory.definitions(), nullptr, nullptr).ok());
    assert(false_service.initialize(target_false).ok());
    assert(false_service.seed_features().result.ok());
    JsonValue false_api_namespace, false_security_namespace;
    assert(false_service.read_feature_namespace("core.api", &false_api_namespace).ok());
    assert(false_service.read_feature_namespace("module.security", &false_security_namespace).ok());
    const JsonValue* false_api_features = json_member(false_api_namespace, "features");
    const JsonValue* false_security_features = json_member(false_security_namespace, "features");
    assert(!json_member(*false_api_features, "api.enabled")->boolean);
    assert(!json_member(*false_security_features, "op.enabled")->boolean);

    const auto flat_legacy = root + "/owner-migration-flat-legacy";
    assert(::mkdir(flat_legacy.c_str(), 0700) == 0);
    {
        std::ofstream file(flat_legacy + "/config.json");
        file << R"({"apiEnabled":false,"listenAddress":"0.0.0.0","listenPort":8123,"opEnabled":false,"core.api":{"enable":true,"features":{"core.api.http_registry":true},"config":{}},"module.platform":{"enable":true,"features":{"debug.logging":false},"config":{}},"module.security":{"enable":true,"features":{}}})";
    }
    auto flat_directory = migration_directory();
    CoreSettingsConfigService flat_service;
    assert(flat_service.prepare_features(flat_directory.definitions(), nullptr, nullptr).ok());
    assert(flat_service.initialize(flat_legacy).ok());
    assert(flat_service.seed_features().result.ok());
    JsonValue flat_api_namespace, flat_security_namespace;
    assert(flat_service.read_feature_namespace("core.api", &flat_api_namespace).ok());
    assert(flat_service.read_feature_namespace("module.security", &flat_security_namespace).ok());
    const JsonValue* flat_api_features = json_member(flat_api_namespace, "features");
    const JsonValue* flat_security_features = json_member(flat_security_namespace, "features");
    const JsonValue* flat_api_config = json_member(flat_api_namespace, "config");
    assert(!json_member(*flat_api_features, "api.enabled")->boolean);
    assert(!json_member(*flat_security_features, "op.enabled")->boolean);
    assert(json_member(*flat_api_config, "listen_address")->scalar == "0.0.0.0");
    assert(json_member(*flat_api_config, "listen_port")->scalar == "8123");
    const std::string flat_persisted = bytes(flat_legacy + "/config.json");
    assert(flat_persisted.find("apiEnabled") == std::string::npos &&
           flat_persisted.find("opEnabled") == std::string::npos &&
           flat_persisted.find("listenAddress") == std::string::npos &&
           flat_persisted.find("listenPort") == std::string::npos);

    const auto invalid = root + "/owner-migration-invalid-value";
    write_migration_file(invalid, true, true, false, false, false, false, false);
    {
        std::ofstream file(invalid + "/config.json", std::ios::trunc);
        file << R"({"module.platform":{"enable":true,"features":{"debug.logging":false,"api.enabled":true},"config":{"listen_port":70000}},"core.api":{"enable":true,"features":{"core.api.http_registry":true},"config":{}},"module.security":{"enable":true,"features":{}}})";
    }
    const std::string invalid_original = bytes(invalid + "/config.json");
    auto invalid_directory = migration_directory();
    CoreSettingsConfigService invalid_service;
    assert(invalid_service.prepare_features(invalid_directory.definitions(), nullptr, nullptr).ok());
    assert(invalid_service.initialize(invalid).ok());
    const auto invalid_seed = invalid_service.seed_features();
    assert(invalid_seed.result.error == ConfigStoreError::kValidation &&
           !invalid_seed.memory_committed && !invalid_seed.durable_commit);
    assert(bytes(invalid + "/config.json") == invalid_original);
    assert(invalid_service.revision() == 1);
}

void test_failed_host_migration_is_atomic(const std::string& root) {
    const auto path = root + "/owner-migration-failed-write";
    write_migration_file(path, false, true, false, false, false, false, false);
    const std::string original = bytes(path + "/config.json");
    auto directory = migration_directory();
    CoreSettingsConfigService service;
    assert(service.prepare_features(directory.definitions(), nullptr, nullptr).ok());
    assert(service.initialize(path).ok());
    assert(::rename((path + "/config.json").c_str(), (path + "/saved.json").c_str()) == 0);
    assert(::mkdir((path + "/config.json").c_str(), 0700) == 0);
    const auto seeded = service.seed_features();
    assert(seeded.result.error == ConfigStoreError::kPersistence &&
           seeded.changed && !seeded.memory_committed && !seeded.durable_commit);
    assert(bytes(path + "/saved.json") == original);
    assert(service.revision() == 1);
    JsonValue namespace_value;
    assert(!service.read_feature_namespace("core.api", &namespace_value).ok());
    std::vector<PluginDesiredSnapshot> desired;
    assert(service.desired_snapshots(&desired).error == ConfigStoreError::kNotReady);
}
}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    static_assert(std::is_standard_layout_v<FeatureDefinition> &&
                  std::is_trivially_copyable_v<FeatureDefinition>);
    static_assert(std::is_standard_layout_v<QolFeatureApi> && std::is_trivial_v<QolFeatureApi>);
    FeatureDirectory directory;
    auto invalid = descriptor("invalid");
    invalid.feature_count = 0;
    assert(!directory.add(invalid).ok());
    invalid.feature_count = 2;
    invalid.features = nullptr;
    assert(!directory.add(invalid).ok());
    FeatureDefinition bad[] = {definitions[0], definitions[0]};
    invalid.features = bad;
    assert(!directory.add(invalid).ok());
    bad[1] = definitions[1];
    bad[0].default_enabled = 2;
    assert(!directory.add(invalid).ok());
    bad[0] = definitions[0];
    bad[1].type = static_cast<FeatureType>(9);
    assert(!directory.add(invalid).ok());
    bad[1] = definitions[1];
    bad[0].display_priority = 11;
    auto out_of_range = directory.add(invalid);
    assert(!out_of_range.ok() && out_of_range.message == "feature display priority out of range");
    bad[0].display_priority = -11;
    out_of_range = directory.add(invalid);
    assert(!out_of_range.ok() && out_of_range.message == "feature display priority out of range");
    bad[0] = definitions[0];
    bad[0].display_priority = -10;
    bad[1].display_priority = 10;
    FeatureDirectory boundary_directory;
    auto boundary_descriptor = descriptor("priority.boundaries");
    boundary_descriptor.features = bad;
    assert(boundary_directory.add(boundary_descriptor).ok());
    bad[0] = definitions[0];
    bad[1] = definitions[1];
    bad[1].on_entry = nullptr;
    assert(!directory.add(invalid).ok());
    assert(directory.add(descriptor("plugin.alpha")).ok());
    assert(!directory.add(descriptor("plugin.alpha")).ok());
    assert(directory.add(descriptor("plugin.beta")).ok());

    HttpEndpointDescriptor endpoint{};
    endpoint.method = HttpMethod::kGet;
    endpoint.path = "/test/query";
    endpoint.route_owner = "test";
    endpoint.kind = HttpHandlerKind::kQuery;
    endpoint.query_handler = &endpoint_handler;
    auto endpoint_plugin = descriptor("plugin.endpoint");
    endpoint_plugin.http_endpoints = &endpoint;
    endpoint_plugin.http_endpoint_count = 1;
    FeatureDirectory endpoint_directory;
    assert(endpoint_directory.add(endpoint_plugin).ok());
    endpoint.method = HttpMethod::kPost;
    assert(!FeatureDirectory{}.add(endpoint_plugin).ok());
    endpoint.method = HttpMethod::kGet;
    endpoint.command_handler = &endpoint_handler;
    assert(!FeatureDirectory{}.add(endpoint_plugin).ok());

    std::string path = std::string(argv[1]) + "/feature-controller-XXXXXX";
    assert(::mkdtemp(path.data()));
    test_seed_type_validation(path, directory);
    test_immutable_reentry(path);
    test_registered_projection(path);
    test_feature_writes_derive_enable_atomically(path);
    test_host_owner_migration(path);
    test_failed_host_migration_is_atomic(path);
    test_receipt_push(directory, path);
    {
        std::ofstream file(path + "/config.json");
        file << R"({"plugin.alpha":{"enable":true,"features":{"toggle":false,"obsolete":true},"config":{"unknown":{"array":[1,null,"x"]}}},"unregistered":{"config":{"keep":true}}})";
    }
    CoreSettingsConfigService service;
    FeatureController controller(service);
    assert(service.revision() == 0 && controller.catalog().empty());
    Pending state;
    state.service = &service;
    assert(controller.initialize(directory, path, &pending, &state).ok());
    assert(service.revision() == 2);
    assert(service.seed_features().result.ok() && service.revision() == 2);
    ModuleConfig legacy;
    assert(!service.snapshot(&legacy).ok());
    assert(!service.update_namespace_key("plugin.alpha", "plugin.alpha", "enable", "false").ok());
    assert(!service.replace_namespace("plugin.alpha", "{\"enable\":false}").ok());
    assert(!service.clear_namespace("plugin.alpha").ok());
    auto persisted = bytes(path + "/config.json");
    assert(persisted.find("metadata-name") == std::string::npos);
    assert(persisted.find("metadata-description") == std::string::npos);
    assert(persisted.find("entry-name") == std::string::npos);
    assert(persisted.find("unregistered") != std::string::npos);
    assert(persisted.find("\"unknown\":{\"array\":[1,null,\"x\"]}") != std::string::npos);
    assert(persisted.find("core.settings") == std::string::npos);
    std::vector<PluginDesiredSnapshot> snapshots;
    assert(controller.desired(&snapshots).ok());
    assert(snapshots.size() == 2 && snapshots[0].desired_enable && !snapshots[0].any_feature);
    assert(snapshots[1].want_run()); // Registered default true; unknown obsolete true did not count.

    int alpha_identity = 0, beta_identity = 0, settings_identity = 0, admin_identity = 0;
    auto alpha = owner(&alpha_identity, "plugin.alpha");
    auto beta = owner(&beta_identity, "plugin.beta");
    auto settings = owner(&settings_identity, "core.settings");
    auto admin = owner(&admin_identity, "plugin.manager");
    auto& api = controller.api();
    char buffer[8192]{};
    uint32_t size = 0;
    assert(succeeded(api.read_namespace(api.context, alpha, "plugin.alpha", buffer, sizeof(buffer), &size)));
    std::string own(buffer, size);
    assert(own.find("\"enable\"") == std::string::npos && own.find("\"config\"") != std::string::npos);
    assert(api.read_namespace(api.context, alpha, "plugin.beta", buffer, sizeof(buffer), &size).code == kQolErrBlocked);
    assert(api.read_catalog(api.context, settings, buffer, sizeof(buffer), &size).code == kQolErrBlocked);
    assert(succeeded(controller.grant_admin(admin)));
    assert(succeeded(api.read_catalog(api.context, admin, buffer, sizeof(buffer), &size)));
    assert(std::string(buffer, size).find("metadata-name") != std::string::npos);
    assert(succeeded(api.read_namespace(api.context, admin, "plugin.beta", buffer, sizeof(buffer), &size)));
    uint8_t enabled = 9, any = 9;
    assert(api.read_desired(api.context, alpha, "plugin.alpha", &enabled, &any).code == kQolErrBlocked);
    assert(succeeded(api.read_feature(api.context, alpha, "plugin.alpha", "toggle", &enabled)) && !enabled);
    assert(api.read_feature(api.context, alpha, "plugin.alpha", "obsolete", &enabled).code == kQolErrNotFound);
    FeatureReceipt receipt{};
    assert(api.request_desired(api.context, alpha, "plugin.alpha", 0, &receipt).code == kQolErrBlocked);
    assert(receipt.admission == FeatureAdmission::kRejected && receipt.error_code == kQolErrBlocked);
    assert(api.request_set_feature(api.context, alpha, "plugin.alpha", "enable", 1, &receipt).code == kQolErrNotFound);
    assert(api.request_set_feature(api.context, alpha, "plugin.alpha", "toggle", 2, &receipt).code == kQolErrInvalidArgument);
    assert(api.request_set_config(api.context, alpha, "plugin.beta", "new", "true", 4, &receipt).code == kQolErrBlocked);
    assert(api.request_set_config(api.context, alpha, "plugin.alpha", "bad", "{", 1, &receipt).code == kQolErrInvalidArgument);

    // Hold the pending notification to demonstrate UI admission does not wait for worker I/O/effects.
    { std::lock_guard<std::mutex> lock(state.mutex); state.block = true; }
    const auto main_thread = std::this_thread::get_id();
    assert(succeeded(api.request_set_feature(api.context, alpha, "plugin.alpha", "entry", 1, &receipt)));
    assert(receipt.admission == FeatureAdmission::kAdmitted && receipt.persistence == FeaturePersistence::kPending && receipt.actual == FeatureActual::kNotSubmitted);
    {
        std::unique_lock<std::mutex> lock(state.mutex);
        assert(state.changed.wait_for(lock, std::chrono::seconds(3), [&] { return state.entered; }));
    }
    FeatureReceipt second{};
    assert(succeeded(api.request_set_config(api.context, admin, "plugin.alpha", "arbitrary", "[false,{}]", 10, &second)));
    assert(second.persistence == FeaturePersistence::kPending);
    assert(api.receipt(api.context, beta, receipt.request_id, &second).code == kQolErrBlocked);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.release = true;
    }
    state.changed.notify_all();
    receipt = await(controller, receipt);
    second = await(controller, second);
    assert(receipt.persistence == FeaturePersistence::kPersisted && receipt.actual == FeatureActual::kPending);
    assert(second.persistence == FeaturePersistence::kPersisted && second.revision == receipt.revision + 1);
    { std::lock_guard<std::mutex> lock(state.mutex); assert(state.callback_thread != main_thread); }
    assert(controller.desired(&snapshots).ok() && snapshots[0].want_run()); // Entry bool counts in any.
    int entry_requests = 0;
    assert(controller.invoke_entry("plugin.alpha", "entry").code == kQolErrNotReady);
    controller.bind_entry_admission(&entry_admission, &entry_requests);
    assert(succeeded(api.invoke_entry(api.context, alpha, "plugin.alpha", "entry")));
    assert(entry_requests == 1 && business_calls == 0); // No callback executed on this thread or worker.

    assert(succeeded(api.request_set_config(api.context, alpha, "plugin.alpha", "enable", "false", 5, &receipt)));
    assert(await(controller, receipt).persistence == FeaturePersistence::kPersisted);
    assert(succeeded(api.read_desired(api.context, admin, "plugin.alpha", &enabled, &any)) && enabled && any);
    // config.enable is arbitrary user data, not the Host desired control.
    assert(succeeded(api.request_desired(api.context, admin, "plugin.alpha", 0, &receipt)));
    await(controller, receipt);
    assert(controller.desired(&snapshots).ok() && !snapshots[0].want_run() && snapshots[0].any_feature);
    assert(succeeded(controller.request_desired("plugin.alpha", 1, &receipt)));
    await(controller, receipt);
    assert(succeeded(controller.request_set_feature("plugin.alpha", "entry", 0, &receipt)));
    await(controller, receipt);
    assert(controller.desired(&snapshots).ok() && !snapshots[0].any_feature && !snapshots[0].desired_enable);
    assert(controller.invoke_entry("plugin.alpha", "entry").code == kQolErrBlocked);

    // A callback exception is distinct from durable failure; later events still drain in order.
    { std::lock_guard<std::mutex> lock(state.mutex); state.throw_next = true; }
    assert(succeeded(controller.request_set_feature("plugin.alpha", "toggle", 1, &receipt)));
    receipt = await(controller, receipt);
    assert(receipt.persistence == FeaturePersistence::kPersisted && receipt.error_code == kQolErrInternal);
    { std::lock_guard<std::mutex> lock(state.mutex); state.reenter = true; }
    assert(succeeded(controller.request_set_feature("plugin.alpha", "toggle", 0, &receipt)));
    await(controller, receipt);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        assert(state.nested.committed && state.nested.apply_state == SettingsConfigApplyState::kPendingFrame);
        for (size_t i = 1; i < state.revisions.size(); ++i) assert(state.revisions[i] == state.revisions[i - 1] + 1);
    }

    owner_suspend(&admin_identity);
    assert(api.read_namespace(api.context, admin, "plugin.alpha", buffer, sizeof(buffer), &size).code != 0);
    owner_suspend(&alpha_identity);
    assert(api.request_set_feature(api.context, alpha, "plugin.alpha", "toggle", 1, &receipt).code != 0);
    assert(succeeded(controller.request_set_feature("plugin.alpha", "toggle", 1, &receipt)));
    await(controller, receipt); // Resident Host path needs neither inactive settings nor owner token.
    assert(succeeded(controller.read_feature("plugin.alpha", "toggle", &enabled)) && enabled);
    assert(succeeded(controller.read_config("plugin.alpha", "arbitrary", buffer, sizeof(buffer), &size)));
    assert(std::string(buffer, size) == "[false,{}]");

    assert(succeeded(controller.request_set_feature("plugin.alpha", "entry", 0, &receipt)));
    await(controller, receipt);
    const auto revision = service.revision();
    size_t effects;
    { std::lock_guard<std::mutex> lock(state.mutex); effects = state.revisions.size(); }
    assert(::rename((path + "/config.json").c_str(), (path + "/saved.json").c_str()) == 0);
    assert(::mkdir((path + "/config.json").c_str(), 0700) == 0);
    ReceiptNotice persistence_failure_notice;
    uint64_t persistence_failure_subscription = 0;
    assert(succeeded(controller.request_set_feature("plugin.alpha", "toggle", 0, &receipt)));
    assert(succeeded(controller.subscribe_receipt(receipt.request_id, &record_receipt,
                                                  &persistence_failure_notice,
                                                  &persistence_failure_subscription)));
    receipt = await(controller, receipt);
    assert(receipt.persistence == FeaturePersistence::kFailed && receipt.actual == FeatureActual::kNotSubmitted);
    {
        std::unique_lock<std::mutex> lock(persistence_failure_notice.mutex);
        assert(persistence_failure_notice.changed.wait_for(
            lock, std::chrono::seconds(4), [&] { return persistence_failure_notice.completed; }));
        assert(persistence_failure_notice.calls == 1);
        assert(persistence_failure_notice.receipt.persistence == FeaturePersistence::kFailed);
    }
    assert(feature_receipt_json(persistence_failure_notice.receipt) == feature_receipt_json(receipt));
    assert(service.revision() == revision);
    uint8_t toggle_value = 0, entry_value = 1;
    assert(succeeded(controller.read_feature("plugin.alpha", "toggle", &toggle_value)) && toggle_value);
    assert(succeeded(controller.read_feature("plugin.alpha", "entry", &entry_value)) && !entry_value);
    assert(controller.desired(&snapshots).ok() && snapshots[0].desired_enable && snapshots[0].any_feature);
    { std::lock_guard<std::mutex> lock(state.mutex); assert(state.revisions.size() == effects); }
    assert(business_calls == 0);
    std::puts("host_feature_controller_tests: pass (schema/defaults/existing/metadata, enable isolation, own/admin leases, async receipts/failure/revisions/reentry, entry boolean, zero-feature invalid)");
}
