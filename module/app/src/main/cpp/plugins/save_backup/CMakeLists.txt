add_library(qol_plugin_save_backup STATIC
    api/json_codec.cpp
    backup_files.cpp
    bundle.cpp
    feature.cpp
    plugin.cpp
    persistence/persistence.cpp
    port/save_port.cpp
    settings_contribution.cpp
    state/state.cpp
    transaction/transactions.cpp
    usecases/backup_usecases.cpp
    usecases/meta_backfill.cpp
    ui/panel.cpp
)

set_target_properties(qol_plugin_save_backup PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
