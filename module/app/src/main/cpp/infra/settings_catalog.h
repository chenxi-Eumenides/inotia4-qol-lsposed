#pragma once

#include "contracts/settings.h"

namespace qol::infra {

// Owning copy for host dispatch; infra stores callbacks but never executes them.
struct SettingsActionRecord {
    contracts::SettingsItemView metadata;
    contracts::SettingsOpenFn open;
    contracts::SettingsAvailableFn available;
    void* user;
};

QolResult settings_catalog_register_category(const contracts::SettingsCategorySpec* category,
                                             const contracts::SettingsItemSpec* items,
                                             uint32_t count);
QolResult settings_catalog_register_category_metadata(
    const contracts::SettingsCategorySpec* category);
QolResult settings_catalog_register_items(const char* owner, const char* category_id,
                                         const contracts::SettingsItemSpec* items,
                                         uint32_t count);
QolResult settings_catalog_unregister_owner(const char* owner);
QolResult settings_catalog_snapshot_acquire(const contracts::CatalogView** out_view,
                                            contracts::SettingsCatalogToken* out_token);
QolResult settings_catalog_snapshot_release(contracts::SettingsCatalogToken token);
QolResult settings_catalog_resolve_action(const char* item_id, uint64_t generation,
                                          SettingsActionRecord* out);
QolResult settings_catalog_resolve_item(const char* item_id, uint64_t generation,
                                         SettingsActionRecord* out);
void settings_catalog_reset_for_test();

}  // namespace qol::infra
