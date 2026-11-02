#include "host/config_namespace.h"
#include "host/config_platform_bridge.h"
#include "host/core_settings_config_service.h"
#include "infra/commands.h"
#include "infra/log/qol_log.h"

#include <cassert>
#include <cstdio>
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

namespace {

QolResult fake_read(void*, const char*, char*, uint32_t, uint32_t*) {
    return qol::contracts::ok();
}

QolResult fake_replace(void*, const char*, const char*, uint32_t) {
    return qol::contracts::ok();
}

QolResult fake_clear(void*, const char*) { return qol::contracts::ok(); }

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    int fake_context = 0;
    assert(qol::host::config_namespace_bind_backend(&fake_context, &fake_read, &fake_replace,
                                                    &fake_clear));
    std::string root = std::string(argv[1]) + "/config-platform-conflict-XXXXXX";
    assert(::mkdtemp(root.data()) != nullptr);
    const std::string directory = root + "/settings";
    assert(::mkdir(directory.c_str(), 0700) == 0);

    assert(qol::host::config_store_bootstrap(directory) == -1);
    assert(!qol::host::config_store_bootstrapped());
    assert(qol::host::core_settings_config_service().state() ==
           qol::host::CoreSettingsConfigServiceState::kReady);
    assert(qol::host::config_store_bootstrap(directory) == -1);
    assert(!qol::host::config_store_bootstrapped());
    qol::host::config_namespace_reset_for_test();
    assert(qol::host::config_store_bootstrap(directory) == 0);
    assert(qol::host::config_store_bootstrapped());
    std::puts("config_platform_bootstrap_conflict_tests: pass");
}
