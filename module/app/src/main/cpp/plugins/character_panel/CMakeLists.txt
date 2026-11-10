add_library(qol_plugin_character_panel STATIC
    feature.cpp
    plugin.cpp
    charinfo_scope.cpp
    charinfo_watch.cpp
    overview.cpp
)

set_target_properties(qol_plugin_character_panel PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
