#include "host/core_settings_config_store.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace qol::features::core_settings;

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    assert(output.good());
}

std::string make_dir(const std::string& root, const char* name) {
    const std::string path = root + "/" + name;
    assert(::mkdir(path.c_str(), 0700) == 0);
    return path;
}

const JsonValue* field(const JsonValue& object, const std::string& key) {
    for (const auto& entry : object.object) if (entry.first == key) return &entry.second;
    return nullptr;
}

bool bool_field(const JsonValue& object, const std::string& key) {
    const JsonValue* value = field(object, key);
    assert(value != nullptr && value->kind == JsonValue::Kind::kBoolean);
    return value->boolean;
}

int int_field(const JsonValue& object, const std::string& key) {
    const JsonValue* value = field(object, key);
    assert(value != nullptr && value->kind == JsonValue::Kind::kNumber);
    return std::stoi(value->scalar);
}

std::vector<std::string> root_keys(const std::string& json) {
    std::vector<std::string> keys;
    std::size_t i = json.find('{');
    assert(i != std::string::npos);
    ++i;
    while (i < json.size()) {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' ||
                                   json[i] == '\t' || json[i] == ',')) ++i;
        if (i >= json.size() || json[i] == '}') break;
        assert(json[i] == '"');
        const std::size_t start = ++i;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\') ++i;
            ++i;
        }
        assert(i < json.size());
        keys.emplace_back(json.substr(start, i - start));
        ++i;
        while (i < json.size() && json[i] != ':') ++i;
        assert(i < json.size());
        ++i;
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (; i < json.size(); ++i) {
            const char ch = json[i];
            if (in_string) {
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') in_string = false;
                continue;
            }
            if (ch == '"') in_string = true;
            else if (ch == '{' || ch == '[') ++depth;
            else if (ch == '}' || ch == ']') {
                if (depth == 0) break;
                --depth;
            } else if (ch == ',' && depth == 0) break;
        }
        if (i < json.size() && json[i] == '}') break;
        if (i < json.size() && json[i] == ',') ++i;
    }
    return keys;
}

void test_legacy_migration_and_unknown_preservation(const std::string& root) {
    const std::string directory = make_dir(root, "legacy-migration");
    const std::string input =
        "{\"listenAddress\":\"legacy-address\",\"listenPort\":1234,"
        "\"stackLimitIncrease\":false,\"moveMergeEnabled\":true,\"opEnabled\":true,"
        "\"extensionBagEnabled\":true,\"customRecipeEnabled\":true,"
        "\"simpleModeEnabled\":true,\"pushAllEnabled\":true,\"autoSellEnabled\":true,"
        "\"apiEnabled\":false,\"debugLogEnabled\":true,\"jewelBatchMix\":true,"
        "\"core.settings\":{\"enable\":true,\"listenAddress\":\"192.0.2.8\","
        "\"listenPort\":4321,\"opEnabled\":false,\"apiEnabled\":true,"
        "\"debugLogEnabled\":false,\"custom\":{\"keep\":true}},"
        "\"stack_limit\":{\"enable\":true,\"limit\":999},"
        "\"extension_bag\":{\"enable\":false,\"capacity\":16},"
        "\"future.scalar\":17,\"future.array\":[true,null,{\"x\":2}],"
        "\"future.object\":{\"nested\":[\"ok\"]},\"plugin.unregistered\":{\"enable\":false},"
        "\"future.scalar\":18}";
    const std::string path = directory + "/config.json";
    write_file(path, input);

    ConfigStore store;
    assert(store.init(directory).ok());
    assert(store.registered_plugin_ids().empty());
    assert(store.snapshot().listen_port == 8088 && !store.snapshot().stack_limit_increase);
    JsonValue hidden_legacy;
    assert(store.read_namespace("stack_limit", &hidden_legacy).error == ConfigStoreError::kInvalidArgument);
    const std::string migrated_before_seed = read_file(path);
    assert(migrated_before_seed.find("\"stack_limit\":{\"enable\":true") != std::string::npos);
    assert(migrated_before_seed.find("\"listenAddress\":\"legacy-address\"") == std::string::npos);
    assert(store.seed_registered_plugins({"autosell", "core.settings", "custom_recipe", "extension_bag",
                                          "move_merge", "push_all", "simple_mode", "stack_limit"}).result.ok());
    const ModuleConfig projection = store.snapshot();
    assert(projection.listen_address == "192.0.2.8" && projection.listen_port == 4321);
    assert(!projection.op_enabled && projection.api_enabled && !projection.debug_log_enabled);
    assert(projection.stack_limit_increase && projection.move_merge_enabled);
    assert(!projection.extension_bag_enabled && projection.custom_recipe_enabled);
    assert(projection.simple_mode_enabled && projection.push_all_enabled && projection.auto_sell_enabled);

    JsonValue core;
    assert(store.read_namespace("core.settings", &core).ok());
    assert(bool_field(core, "enable") && bool_field(core, "apiEnabled"));
    assert(field(core, "custom") != nullptr);
    JsonValue stack;
    assert(store.read_namespace("stack_limit", &stack).ok());
    assert(bool_field(stack, "enable") && int_field(stack, "limit") == 999);
    JsonValue move_merge;
    assert(store.read_namespace("move_merge", &move_merge).ok() && bool_field(move_merge, "enable"));
    JsonValue extension_bag;
    assert(store.read_namespace("extension_bag", &extension_bag).ok());
    assert(!bool_field(extension_bag, "enable") && int_field(extension_bag, "capacity") == 16);
    JsonValue recipe;
    JsonValue simple;
    JsonValue push;
    JsonValue autosell;
    assert(store.read_namespace("custom_recipe", &recipe).ok() && bool_field(recipe, "enable"));
    assert(store.read_namespace("simple_mode", &simple).ok() && bool_field(simple, "enable"));
    assert(store.read_namespace("push_all", &push).ok() && bool_field(push, "enable"));
    assert(store.read_namespace("autosell", &autosell).ok() && bool_field(autosell, "enable"));
    JsonValue hidden;
    assert(store.read_namespace("plugin.unregistered", &hidden).error == ConfigStoreError::kInvalidArgument);
    assert(store.seed_registered_plugins({"core.settings"}).result.ok());
    assert(store.registered_plugin_ids() == std::vector<std::string>{"core.settings"});
    assert(store.read_namespace("stack_limit", &hidden).error == ConfigStoreError::kInvalidArgument);
    assert(!store.snapshot().stack_limit_increase);
    assert(!store.update_namespace_key("core.settings", "stack_limit", "enable", "false").ok());
    assert(read_file(path).find("\"stack_limit\":{\"enable\":true") != std::string::npos);
    char registered_json[4096]{};
    uint32_t registered_size = 0;
    assert(store.read_registered_json(registered_json, sizeof(registered_json), &registered_size).ok());
    assert(std::string(registered_json, registered_size).find("stack_limit") == std::string::npos);
    assert(store.seed_registered_plugins({"autosell", "core.settings", "custom_recipe", "extension_bag",
                                          "move_merge", "push_all", "simple_mode", "stack_limit"}).result.ok());
    assert(store.snapshot().stack_limit_increase);

    const std::string migrated = read_file(path);
    const auto top_keys = root_keys(migrated);
    for (const char* flat : {"listenAddress", "listenPort", "stackLimitIncrease", "moveMergeEnabled",
                             "opEnabled", "extensionBagEnabled", "customRecipeEnabled",
                             "simpleModeEnabled", "pushAllEnabled", "autoSellEnabled", "apiEnabled",
                             "debugLogEnabled", "jewelBatchMix"})
        assert(std::find(top_keys.begin(), top_keys.end(), flat) == top_keys.end());
    for (const char* unknown : {"future.scalar", "future.array", "future.object", "plugin.unregistered"})
        assert(migrated.find(unknown) != std::string::npos);
    assert(migrated.find("\"future.scalar\":18") != std::string::npos);
    assert(migrated.find("\"future.scalar\":17") == std::string::npos);
    const auto names = store.registered_plugin_ids();
    assert(std::is_sorted(names.begin(), names.end()));
    assert(std::find(names.begin(), names.end(), "plugin.unregistered") == names.end());
    assert(top_keys.size() >= names.size());
    for (const std::string& id : names)
        assert(std::find(top_keys.begin(), top_keys.end(), id) != top_keys.end());
    assert(store.update_namespace_key("stack_limit", "stack_limit", "batch", "{\"size\":3}").ok());
    const std::string after_single_key = read_file(path);
    for (const char* unknown : {"future.scalar", "future.array", "future.object", "plugin.unregistered"})
        assert(after_single_key.find(unknown) != std::string::npos);
    const std::string stable = read_file(path);
    ConfigStore restarted;
    assert(restarted.init(directory).ok());
    assert(restarted.registered_plugin_ids().empty());
    assert(restarted.snapshot().listen_port == 8088 && !restarted.snapshot().move_merge_enabled);
    assert(restarted.seed_registered_plugins({"autosell", "core.settings", "custom_recipe",
                                              "extension_bag", "move_merge", "push_all",
                                              "simple_mode", "stack_limit"}).result.ok());
    assert(read_file(path) == stable);
    assert(restarted.snapshot().listen_port == 4321 && restarted.snapshot().move_merge_enabled);
}

void test_seeding_repair_and_unknown_update(const std::string& root) {
    const std::string directory = make_dir(root, "seeding");
    write_file(directory + "/config.json",
        "{\"plugin.custom\":{\"enable\":false,\"keep\":{\"v\":1}},"
        "\"plugin.missing_enable\":{\"x\":2},"
        "\"future.namespace\":{\"payload\":[1,2]},\"future.scalar\":13}");
    ConfigStore store;
    assert(store.init(directory).ok());
    const auto before_batch_ids = store.registered_plugin_ids();
    assert(std::find(before_batch_ids.begin(), before_batch_ids.end(), "plugin.custom") ==
           before_batch_ids.end());
    JsonValue hidden_custom;
    assert(store.read_namespace("plugin.custom", &hidden_custom).error == ConfigStoreError::kInvalidArgument);
    const std::vector<std::string> seed_ids = {"plugin.custom", "plugin.new", "plugin.new",
                                               "plugin.missing_enable"};
    const auto seeded = store.seed_registered_plugins(seed_ids);
    assert(seeded.result.ok() && seeded.result.persisted && seeded.changed && seeded.memory_committed);
    const std::pair<const char*, bool> defaults[] = {{"plugin.new", true}};
    for (const auto& item : defaults) {
        JsonValue object;
        assert(store.read_namespace(item.first, &object).ok());
        assert(bool_field(object, "enable") == item.second);
    }
    JsonValue custom;
    assert(store.read_namespace("plugin.custom", &custom).ok());
    assert(!bool_field(custom, "enable") && field(custom, "keep") != nullptr);
    JsonValue missing_enable;
    assert(store.read_namespace("plugin.missing_enable", &missing_enable).ok());
    assert(bool_field(missing_enable, "enable") && int_field(missing_enable, "x") == 2);
    const auto repeated = store.seed_registered_plugins(seed_ids);
    assert(repeated.result.ok() && !repeated.changed && repeated.result.persisted);
    const auto registered = store.registered_plugin_ids();
    assert(std::find(registered.begin(), registered.end(), "plugin.custom") != registered.end());
    assert(std::find(registered.begin(), registered.end(), "plugin.new") != registered.end());
    assert(std::find(registered.begin(), registered.end(), "plugin.missing_enable") != registered.end());
    assert(std::find(registered.begin(), registered.end(), "future.namespace") == registered.end());

    const auto shrunk = store.seed_registered_plugins({"plugin.custom"});
    assert(shrunk.result.ok());
    assert(store.registered_plugin_ids() == std::vector<std::string>{"plugin.custom"});
    assert(store.read_namespace("plugin.missing_enable", &missing_enable).error == ConfigStoreError::kInvalidArgument);
    assert(store.read_namespace("plugin.new", &custom).error == ConfigStoreError::kInvalidArgument);
    assert(read_file(directory + "/config.json").find("\"plugin.missing_enable\"") != std::string::npos);
    assert(read_file(directory + "/config.json").find("\"future.namespace\"") != std::string::npos);
    assert(store.seed_registered_plugins({"plugin.custom", "plugin.missing_enable", "plugin.new"}).result.ok());
    assert(store.read_namespace("plugin.missing_enable", &missing_enable).ok());

    const std::string path = directory + "/config.json";
    assert(store.update_namespace_key("plugin.custom", "plugin.custom", "settings",
                                      "{\"array\":[1,true,null],\"duplicate\":1,\"duplicate\":2}").ok());
    assert(read_file(path).find("\"future.namespace\":{\"payload\":[1,2]}") != std::string::npos);
    assert(read_file(path).find("\"future.scalar\":13") != std::string::npos);
    assert(store.read_namespace("plugin.custom", &custom).ok());
    const JsonValue* settings = field(custom, "settings");
    assert(settings != nullptr && settings->kind == JsonValue::Kind::kObject);
    const JsonValue* duplicate = field(*settings, "duplicate");
    assert(duplicate != nullptr && duplicate->scalar == "2");
    const std::string persisted = read_file(path);
    assert(persisted.find("\"duplicate\":1") == std::string::npos);
    assert(persisted.find("\"duplicate\":2") != std::string::npos);

    const std::string repair_dir = make_dir(root, "enable-repair");
    write_file(repair_dir + "/config.json",
               "{\"plugin.custom\":{\"enable\":\"bad\",\"keep\":{\"v\":1}}}");
    ConfigStore repair;
    assert(repair.init(repair_dir).ok());
    assert(repair.seed_registered_plugins({"plugin.custom"}).result.ok());
    JsonValue repaired;
    assert(repair.read_namespace("plugin.custom", &repaired).ok());
    assert(bool_field(repaired, "enable"));
    assert(field(repaired, "keep") != nullptr);
}

void test_owner_key_authorization_and_atomic_failures(const std::string& root) {
    const std::string directory = make_dir(root, "single-key");
    ConfigStore store;
    assert(store.init(directory).ok());
    const auto seeded = store.seed_registered_plugins({"plugin.custom", "plugin.other"});
    assert(seeded.result.ok() && seeded.changed);
    assert(store.update_namespace_key("plugin.custom", "plugin.custom", "own", "[1,true,null]").ok());
    assert(store.update_namespace_key("plugin.other", "plugin.custom", "own", "{\"v\":2}").ok());
    assert(!store.update_namespace_key("plugin.other", "plugin.custom", "cross-new", "true").ok());
    assert(!store.update_namespace_key("unregistered", "plugin.custom", "own", "false").ok());
    assert(!store.update_namespace_key("plugin.custom", "not.registered", "own", "false").ok());
    assert(!store.update_namespace_key("plugin.custom", "plugin.custom", "enable", "1").ok());
    assert(store.update_namespace_key("plugin.other", "plugin.custom", "enable", "false").ok());
    JsonValue custom;
    assert(store.read_namespace("plugin.custom", &custom).ok() && !bool_field(custom, "enable"));

    const std::string path = directory + "/config.json";
    const std::string before_invalid = read_file(path);
    const ModuleConfig before_config = store.snapshot();
    std::string invalid_utf8("\"", 1);
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8 += "\"";
    assert(!store.update_namespace_key("plugin.custom", "plugin.custom", "bad", invalid_utf8).ok());
    assert(!store.update_namespace_key("plugin.custom", "plugin.custom", "bad", "{\"x\":}").ok());
    std::string too_deep = "null";
    for (int i = 0; i < 66; ++i) too_deep = "[" + too_deep + "]";
    assert(!store.update_namespace_key("plugin.custom", "plugin.custom", "bad", too_deep).ok());
    assert(!store.update_namespace_key("plugin.custom", "plugin.custom", "bad",
                                      "\"" + std::string(1024 * 1024, 'x') + "\"").ok());
    assert(store.snapshot().listen_port == before_config.listen_port);
    assert(read_file(path) == before_invalid);

    assert(::unlink(path.c_str()) == 0);
    assert(::mkdir(path.c_str(), 0700) == 0);
    const auto failed = store.update_namespace_key("plugin.custom", "plugin.custom", "own", "7");
    assert(!failed.ok() && !failed.persisted && failed.error == ConfigStoreError::kPersistence);
    assert(store.read_namespace("plugin.custom", &custom).ok());
    assert(field(custom, "own")->kind == JsonValue::Kind::kObject);
}

void test_batch_collision_is_atomic(const std::string& root) {
    const std::string directory = make_dir(root, "batch-collision");
    const std::string path = directory + "/config.json";
    write_file(path, "{\"plugin.collision\":17,\"future\":{\"keep\":true}}");
    ConfigStore store;
    assert(store.init(directory).ok());
    const std::string before = read_file(path);
    const auto before_ids = store.registered_plugin_ids();
    const auto collision = store.seed_registered_plugins({"plugin.collision", "plugin.new"});
    assert(!collision.result.ok() && !collision.result.persisted && !collision.memory_committed);
    assert(collision.result.message.find("conflicts with non-object root value") != std::string::npos);
    assert(read_file(path) == before);
    assert(store.registered_plugin_ids() == before_ids);
    JsonValue output;
    assert(store.read_namespace("plugin.collision", &output).error == ConfigStoreError::kInvalidArgument);
}

void test_io_states(const std::string& root) {
    const std::string missing_dir = make_dir(root, "missing-writeback");
    assert(::mkdir((missing_dir + "/config.json.tmp").c_str(), 0700) == 0);
    ConfigStore missing;
    const auto missing_init = missing.init(missing_dir);
    assert(!missing_init.ok() && !missing_init.persisted && !missing.degraded_read_only());
    assert(missing.snapshot().listen_port == 8088);
    const std::vector<std::string> retry_ids = {"core.settings", "plugin.retry"};
    const auto seed_failure = missing.seed_registered_plugins(retry_ids);
    assert(!seed_failure.result.ok() && !seed_failure.result.persisted && seed_failure.changed &&
           seed_failure.memory_committed);
    assert(::access((missing_dir + "/config.json").c_str(), F_OK) != 0);
    JsonValue retry_memory_namespace;
    assert(missing.read_namespace("plugin.retry", &retry_memory_namespace).ok());
    assert(bool_field(retry_memory_namespace, "enable"));
    const auto seeded_ids = missing.registered_plugin_ids();
    assert(std::find(seeded_ids.begin(), seeded_ids.end(), "plugin.retry") != seeded_ids.end());
    assert(::rmdir((missing_dir + "/config.json.tmp").c_str()) == 0);
    ModuleConfigPatch patch;
    patch.listen_port = 9999;
    assert(missing.update_namespace_key("plugin.retry", "plugin.retry", "persisted", "true").ok());
    const auto retry = missing.update(patch);
    assert(retry.ok() && retry.persisted && missing.snapshot().listen_port == 9999);
    ConfigStore restarted_missing;
    assert(restarted_missing.init(missing_dir).ok());
    const auto claim_retry = restarted_missing.seed_registered_plugins(retry_ids);
    assert(claim_retry.result.ok());
    JsonValue retry_namespace;
    assert(restarted_missing.read_namespace("plugin.retry", &retry_namespace).ok());
    assert(bool_field(retry_namespace, "enable") && bool_field(retry_namespace, "persisted"));

    const std::string corrupt_dir = make_dir(root, "corrupt-writeback");
    write_file(corrupt_dir + "/config.json", "{broken");
    assert(::mkdir((corrupt_dir + "/config.json.tmp").c_str(), 0700) == 0);
    ConfigStore corrupt;
    const auto corrupt_init = corrupt.init(corrupt_dir);
    assert(!corrupt_init.ok() && !corrupt_init.persisted && !corrupt.degraded_read_only());
    assert(corrupt.snapshot().listen_port == 8088);
    const auto corrupt_seed = corrupt.seed_registered_plugins({"core.settings"});
    assert(!corrupt_seed.result.ok() && corrupt_seed.memory_committed);
    assert(::rmdir((corrupt_dir + "/config.json.tmp").c_str()) == 0);
    assert(corrupt.update(patch).ok() && corrupt.snapshot().listen_port == 9999);

    const std::string blocker = root + "/not-directory";
    write_file(blocker, "preserve");
    ConfigStore hard_failure;
    assert(!hard_failure.init(blocker + "/settings").ok());
    assert(hard_failure.degraded_read_only() && hard_failure.snapshot().listen_port == 8088);
    const auto degraded_seed = hard_failure.seed_registered_plugins({"plugin.hard"});
    assert(!degraded_seed.result.ok() && !degraded_seed.result.persisted &&
           degraded_seed.memory_committed);
    JsonValue degraded_plugin;
    const auto plugin_read = hard_failure.read_namespace("plugin.hard", &degraded_plugin);
    assert(plugin_read.ok() && !plugin_read.persisted && bool_field(degraded_plugin, "enable"));
    JsonValue degraded_core;
    assert(hard_failure.read_namespace("core.settings", &degraded_core).error == ConfigStoreError::kNotReady);
    JsonValue unavailable_namespace;
    assert(hard_failure.read_namespace("plugin.unknown", &unavailable_namespace).error ==
           ConfigStoreError::kNotReady);
    assert(hard_failure.update(patch).error == ConfigStoreError::kPersistence);
    assert(hard_failure.update_namespace_key("stack_limit", "stack_limit", "enable", "true").error ==
           ConfigStoreError::kPersistence);
    assert(read_file(blocker) == "preserve");

    const std::string nonregular_dir = make_dir(root, "nonregular");
    const std::string config_dir = nonregular_dir + "/config.json";
    assert(::mkdir(config_dir.c_str(), 0700) == 0);
    write_file(config_dir + "/keep", "directory-data");
    ConfigStore nonregular;
    assert(!nonregular.init(nonregular_dir).ok() && nonregular.degraded_read_only());
    const auto nonregular_seed = nonregular.seed_registered_plugins({"core.settings", "extension_bag"});
    assert(!nonregular_seed.result.ok() && nonregular_seed.memory_committed);
    JsonValue nonregular_core;
    const auto nonregular_core_read = nonregular.read_namespace("core.settings", &nonregular_core);
    assert(nonregular_core_read.ok() && !nonregular_core_read.persisted &&
           int_field(nonregular_core, "listenPort") == 8088);
    JsonValue nonregular_extension;
    const auto nonregular_extension_read = nonregular.read_namespace("extension_bag", &nonregular_extension);
    assert(nonregular_extension_read.ok() && !nonregular_extension_read.persisted &&
           bool_field(nonregular_extension, "enable"));
    assert(nonregular.read_namespace("future.plugin", &unavailable_namespace).error ==
           ConfigStoreError::kNotReady);
    assert(nonregular.update(patch).error == ConfigStoreError::kPersistence);
    assert(nonregular.update_namespace_key("core.settings", "core.settings", "apiEnabled", "false").error ==
           ConfigStoreError::kPersistence);
    assert(read_file(config_dir + "/keep") == "directory-data");
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    std::string root = std::string(argv[1]) + "/config-store-namespaced-XXXXXX";
    assert(::mkdtemp(root.data()) != nullptr);
    test_legacy_migration_and_unknown_preservation(root);
    test_seeding_repair_and_unknown_update(root);
    test_owner_key_authorization_and_atomic_failures(root);
    test_batch_collision_is_atomic(root);
    test_io_states(root);
    std::puts("core_settings_config_store_tests: pass");
}
