#pragma once

#include "host/features/reconciler.h"

namespace qol::host {

// All accessors refer to one Loader-lifetime instance backed by the existing service.
// Construction performs no file I/O, starts no worker and selects no Store model.
const FeatureDirectory& production_feature_directory();
FeatureController& production_feature_controller();
FeatureReconciler& production_feature_reconciler();
const contracts::QolFeatureApi& production_feature_api();
const contracts::QolFeatureControlApi& production_feature_control_api();

// Bootstrap thread only, before exposing management readers. Explicit metadata only;
// registration is sealed by bootstrap even if initialization fails.
ConfigStoreResult production_feature_register(const contracts::PluginDescriptor&);
ConfigStoreResult production_feature_register_host_owner(const contracts::PluginDescriptor&);
ConfigStoreResult production_feature_bootstrap(const std::string& external_files_directory);

// Bind/step on the game main thread only, after all registry descriptors are registered.
// The existing reconciler rejects legacy gates; these seams do not call old sync_gates.
bool production_feature_bind_frame(const HostDeps&);
uint32_t production_feature_frame_step();

// Host assembly explicitly selects which SDK copy receives management authority.
// The table remains valid after that UI owner stops; it never consults its old token.
bool production_feature_bind_management_sdk(contracts::QolHostSdk&);

}  // namespace qol::host
