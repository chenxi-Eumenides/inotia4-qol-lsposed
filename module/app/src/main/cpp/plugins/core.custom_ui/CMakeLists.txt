add_library(qol_plugin_core.custom_ui STATIC
    plugin.cpp
    ui_components.cpp
    panels/stack.cpp
)

set_target_properties(qol_plugin_core.custom_ui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
