#pragma once

#include "contracts/module_health.h"

namespace qol::host {

using ModuleHealthObserver = void (*)(
    void* context, const contracts::QolModuleHealthSnapshot& snapshot);

void module_health_set_fact(contracts::QolModuleHealthBit bit, bool known, bool failed);
void module_health_observe_main_menu(bool is_main_menu);
void module_health_config_bootstrap_started();
void module_health_config_bootstrap_finished(bool durable_ready);
const contracts::QolHostModuleHealthFactsApi& module_health_facts_api();
QolResult module_health_subscribe(ModuleHealthObserver observer, void* context,
                                  uint64_t* out_subscription_id);
QolResult module_health_unsubscribe(uint64_t subscription_id);

// Host-only tests reset the resident snapshot between cases.
void module_health_reset_for_test();

}  // namespace qol::host
