#include "host/core_settings_config_service.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using qol::features::core_settings::ConfigStoreError;
using qol::features::core_settings::JsonValue;
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
    const qol::host::CoreSettingsConfigService* first =
        &qol::host::core_settings_config_service();
    const qol::host::CoreSettingsConfigService* second =
        &qol::host::core_settings_config_service();
    assert(first == second);
    auto& service = qol::host::core_settings_config_service();

    ModuleConfig config;
    config.listen_port = 4321;
    const auto before_init_snapshot = service.snapshot(&config);
    assert(!before_init_snapshot.ok() && before_init_snapshot.error == ConfigStoreError::kNotReady);
    assert(before_init_snapshot.message == "configuration service not initialized");
    assert(config.listen_port == 4321);
    ModuleConfigPatch patch;
    patch.listen_port = 9000;
    assert(service.update(patch).error == ConfigStoreError::kNotReady);
    JsonValue namespace_value;
    assert(service.read_namespace("sample.plugin", &namespace_value).error == ConfigStoreError::kNotReady);
    assert(service.replace_namespace("sample.plugin", "true").error == ConfigStoreError::kNotReady);
    assert(service.clear_namespace("sample.plugin").error == ConfigStoreError::kNotReady);
    assert(!service.degraded_read_only());
    assert(service.state() == CoreSettingsConfigServiceState::kUninitialized);
    const auto seed_before_init = service.seed_registered_plugins({"sample.plugin"});
    assert(seed_before_init.result.error == ConfigStoreError::kNotReady);

    std::string root = std::string(argv[1]) + "/run-XXXXXX";
    assert(::mkdtemp(root.data()) != nullptr);
    const std::string directory = root + "/settings";
    assert(::mkdir(directory.c_str(), 0700) == 0);
    const std::string config_directory = directory + "/config.json";
    assert(::mkdir(config_directory.c_str(), 0700) == 0);
    {
        std::ofstream preserved(config_directory + "/keep", std::ios::binary);
        preserved << "preserve";
        assert(preserved.good());
    }

    const auto initialized = service.initialize(directory);
    assert(!initialized.ok() && !initialized.persisted && service.degraded_read_only());
    assert(service.state() == CoreSettingsConfigServiceState::kDegradedReadOnly);
    const auto degraded_seed = service.seed_registered_plugins({"sample.plugin"});
    assert(!degraded_seed.result.ok() && !degraded_seed.result.persisted &&
           degraded_seed.memory_committed);
    assert(service.state() == CoreSettingsConfigServiceState::kDegradedReadOnly);
    config.listen_port = 4321;
    const auto degraded_snapshot = service.snapshot(&config);
    assert(degraded_snapshot.ok() && config.listen_port == 8088);
    assert(config.listen_address == "0.0.0.0" && config.api_enabled && config.extension_bag_enabled);
    assert(service.update(patch).error == ConfigStoreError::kPersistence);
    const auto degraded_namespace = service.read_namespace("sample.plugin", &namespace_value);
    assert(degraded_namespace.ok() && !degraded_namespace.persisted);
    assert(namespace_value.kind == qol::features::core_settings::JsonValue::Kind::kObject);
    assert(service.read_namespace("unknown.plugin", &namespace_value).error == ConfigStoreError::kNotReady);
    assert(service.replace_namespace("sample.plugin", "true").error == ConfigStoreError::kPersistence);
    assert(service.clear_namespace("sample.plugin").error == ConfigStoreError::kPersistence);
    assert(read_file(config_directory + "/keep") == "preserve");

    const auto repeated = service.initialize(directory);
    assert(!repeated.ok() && repeated.message == initialized.message);
    assert(service.state() == CoreSettingsConfigServiceState::kDegradedReadOnly);
    assert(service.snapshot(&config).ok() && config.listen_port == 8088);
    const auto conflict = service.initialize(root + "/other");
    assert(!conflict.ok() && conflict.error == ConfigStoreError::kInvalidArgument);
    assert(service.state() == CoreSettingsConfigServiceState::kDegradedReadOnly);
    assert(service.snapshot(&config).ok() && config.listen_port == 8088);

    std::puts("core_settings_config_service_tests: pass");
    return 0;
}
