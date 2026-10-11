#include "host/features/controller.h"
#include "host/owner_identity.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <type_traits>

using namespace qol;
using namespace qol::contracts;
using namespace qol::host;
using namespace qol::features::core_settings;

namespace {
int business_calls = 0;
QolResult toggle(PluginContext&, uint8_t) { ++business_calls; return ok(); }
QolResult entry(PluginContext&) { ++business_calls; return ok(); }
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
    bad[1].on_entry = nullptr;
    assert(!directory.add(invalid).ok());
    assert(directory.add(descriptor("plugin.alpha")).ok());
    assert(!directory.add(descriptor("plugin.alpha")).ok());
    assert(directory.add(descriptor("plugin.beta")).ok());

    std::string path = std::string(argv[1]) + "/feature-controller-XXXXXX";
    assert(::mkdtemp(path.data()));
    test_seed_type_validation(path, directory);
    test_immutable_reentry(path);
    test_registered_projection(path);
    test_feature_writes_derive_enable_atomically(path);
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
    assert(succeeded(controller.request_set_feature("plugin.alpha", "toggle", 0, &receipt)));
    receipt = await(controller, receipt);
    assert(receipt.persistence == FeaturePersistence::kFailed && receipt.actual == FeatureActual::kNotSubmitted);
    assert(service.revision() == revision);
    uint8_t toggle_value = 0, entry_value = 1;
    assert(succeeded(controller.read_feature("plugin.alpha", "toggle", &toggle_value)) && toggle_value);
    assert(succeeded(controller.read_feature("plugin.alpha", "entry", &entry_value)) && !entry_value);
    assert(controller.desired(&snapshots).ok() && snapshots[0].desired_enable && snapshots[0].any_feature);
    { std::lock_guard<std::mutex> lock(state.mutex); assert(state.revisions.size() == effects); }
    assert(business_calls == 0);
    std::puts("host_feature_controller_tests: pass (schema/defaults/existing/metadata, enable isolation, own/admin leases, async receipts/failure/revisions/reentry, entry boolean, zero-feature invalid)");
}
