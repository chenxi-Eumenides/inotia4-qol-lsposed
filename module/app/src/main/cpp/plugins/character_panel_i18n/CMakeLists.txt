add_library(qol_plugin_character_panel_i18n STATIC
    feature.cpp
    plugin.cpp
    charinfo_scope.cpp
)

set_target_properties(qol_plugin_character_panel_i18n PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
