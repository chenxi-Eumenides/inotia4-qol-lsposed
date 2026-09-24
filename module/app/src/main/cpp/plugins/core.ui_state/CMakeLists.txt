add_library(qol_plugin_core.ui_state STATIC
    feature.cpp
    plugin.cpp
)

target_compile_definitions(qol_plugin_core.ui_state PRIVATE
    QOL_ENABLE_DEVICE_PROBE=$<BOOL:${QOL_ENABLE_DEVICE_PROBE}>
)
