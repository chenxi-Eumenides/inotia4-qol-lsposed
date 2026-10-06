add_library(qol_plugin_save_backup STATIC
    bundle.cpp
    feature.cpp
    plugin.cpp
    settings_contribution.cpp
    ui/panel.cpp
)

set_target_properties(qol_plugin_save_backup PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
