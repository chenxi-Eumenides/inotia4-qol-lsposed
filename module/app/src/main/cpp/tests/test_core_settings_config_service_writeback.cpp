#include "host/core_settings_config_service.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using qol::features::core_settings::ConfigStoreError;
using qol::features::core_settings::ModuleConfig;
using qol::features::core_settings::ModuleConfigPatch;
using qol::host::CoreSettingsConfigServiceState;

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    auto& service = qol::host::core_settings_config_service();
    assert(service.state() == CoreSettingsConfigServiceState::kUninitialized);

    std::string root = std::string(argv[1]) + "/config-service-writeback-XXXXXX";
    assert(::mkdtemp(root.data()) != nullptr);
    const std::string directory = root + "/settings";
    assert(::mkdir(directory.c_str(), 0700) == 0);
    const std::string config_path = directory + "/config.json";
    const std::string temp_path = config_path + ".tmp";
    assert(::mkdir(temp_path.c_str(), 0700) == 0);

    const auto initialized = service.initialize(directory);
    assert(!initialized.ok() && !initialized.persisted);
    assert(service.state() == CoreSettingsConfigServiceState::kReadyUnpersisted);
    assert(!service.degraded_read_only());
    const std::vector<std::string> plugin_ids = {
        "core.settings", "stack_limit", "move_merge", "extension_bag", "custom_recipe",
        "simple_mode", "push_all", "autosell", "plugin.custom"};
    const auto seed_failure = service.seed_registered_plugins(plugin_ids);
    assert(!seed_failure.result.ok() && !seed_failure.result.persisted && seed_failure.memory_committed);
    assert(seed_failure.changed && service.state() == CoreSettingsConfigServiceState::kReadyUnpersisted);
    assert(::access(config_path.c_str(), F_OK) != 0);
    qol::features::core_settings::JsonValue seeded_plugin;
    assert(service.read_namespace("plugin.custom", &seeded_plugin).ok());
    assert(seeded_plugin.kind == qol::features::core_settings::JsonValue::Kind::kObject);

    ModuleConfig config;
    config.listen_port = 1;
    const auto snapshot_result = service.snapshot(&config);
    assert(snapshot_result.ok() && config.listen_address == "0.0.0.0");
    assert(config.listen_port == 8088 && config.api_enabled && config.extension_bag_enabled);

    ModuleConfigPatch patch;
    patch.listen_port = 9090;
    const auto failed_update = service.update(patch);
    assert(!failed_update.ok() && !failed_update.persisted);
    assert(failed_update.error == ConfigStoreError::kPersistence);
    assert(service.state() == CoreSettingsConfigServiceState::kReadyUnpersisted);
    assert(service.snapshot(&config).ok() && config.listen_port == 8088);
    assert(::rmdir(temp_path.c_str()) == 0);

    const auto committed_update = service.update(patch);
    assert(committed_update.ok() && committed_update.persisted);
    assert(service.state() == CoreSettingsConfigServiceState::kReady);
    assert(service.snapshot(&config).ok() && config.listen_port == 9090);
    assert(read_file(config_path).find("\"listenPort\":9090") != std::string::npos);
    const auto repeated_seed = service.seed_registered_plugins(plugin_ids);
    assert(repeated_seed.result.ok() && !repeated_seed.changed && !repeated_seed.durable_commit &&
           service.revision() == 2);
    assert(service.update_namespace_key("plugin.custom", "plugin.custom", "custom", "{\"v\":1}").ok());
    qol::features::core_settings::JsonValue custom;
    assert(service.read_namespace("plugin.custom", &custom).ok());
    assert(service.update_namespace_key("core.settings", "stack_limit", "enable", "true").ok());
    assert(!service.update_namespace_key("core.settings", "stack_limit", "not-present", "true").ok());
    assert(service.snapshot(&config).ok() && config.stack_limit_increase);
    assert(service.revision() == 4);

    std::puts("core_settings_config_service_writeback_tests: pass");
    return 0;
}
