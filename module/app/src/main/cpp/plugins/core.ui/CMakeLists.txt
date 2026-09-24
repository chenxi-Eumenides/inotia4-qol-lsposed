add_library(qol_plugin_core.ui STATIC
    feature.cpp
    plugin.cpp
    ui_components.cpp
    panels/settings.cpp
    panels/settings_ui_local.cpp
    panels/stack.cpp
)

set_target_properties(qol_plugin_core.ui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
