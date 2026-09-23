add_library(qol_plugin_core.api STATIC
    plugin.cpp
    json_field.cpp
)

set_target_properties(qol_plugin_core.api PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
