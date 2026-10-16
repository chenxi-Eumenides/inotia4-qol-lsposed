#include "host/config_platform_bridge.h"
#include "host/core_settings_config_service.h"
#include "infra/commands.h"
#include "infra/log/qol_log.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace qol::host {
void http_dispatch_set_op_enabled(bool) {}
}  // namespace qol::host

void qol_log_set_debug_enabled(bool) {}
void qol_log_write(QolLogLevel, QolDomain, const char*, int, const char*, ...) {}
void frame_cache_start() {}
void frame_cache_stop() {}
namespace qol::infra {
QolResult commands_post_config_bool(const char*, bool, contracts::QolCommandTicket* out) {
    if (out != nullptr) *out = 1;
    return contracts::ok();
}
}  // namespace qol::infra

int main(int argc, char** argv) {
    assert(argc == 2);
    std::string root = std::string(argv[1]) + "/config-platform-degraded-XXXXXX";
    assert(::mkdtemp(root.data()) != nullptr);
    const std::string directory = root + "/settings";
    assert(::mkdir(directory.c_str(), 0700) == 0);
    const std::string config_path = directory + "/config.json";
    assert(::mkdir(config_path.c_str(), 0700) == 0);
    {
        std::ofstream preserved(config_path + "/keep", std::ios::binary);
        preserved << "keep";
        assert(preserved.good());
    }

    assert(qol::host::config_store_bootstrap(directory) == 2);
    assert(qol::host::config_store_bootstrapped());
    auto& service = qol::host::core_settings_config_service();
    assert(service.state() == qol::host::CoreSettingsConfigServiceState::kDegradedReadOnly);
    assert(service.revision() == 1);
    assert(qol::host::config_store_bootstrap(directory) == 2);
    assert(qol::host::config_store_bootstrap(root + "/other") == -1);
    qol::features::core_settings::ModuleConfig config;
    assert(service.snapshot(&config).ok() && config.listen_port == 8088);
    assert(service.update({}).error == qol::features::core_settings::ConfigStoreError::kPersistence);
    std::ifstream preserved(config_path + "/keep", std::ios::binary);
    std::string contents;
    preserved >> contents;
    assert(contents == "keep");
    std::puts("config_platform_degraded_bootstrap_tests: pass");
}
