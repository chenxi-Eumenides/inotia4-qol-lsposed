add_library(qol_plugin_core.settings STATIC
    feature.cpp
    plugin.cpp
    settings_contribution.cpp
    panels/settings.cpp
    panels/settings_binding.cpp
)

set_target_properties(qol_plugin_core.settings PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
