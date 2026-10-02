add_library(qol_plugin_core.custom_ui STATIC
    plugin.cpp
    text_mechanism.cpp
    feature.cpp
    ui_components.cpp
    panels/debug_components.cpp
    panels/stack.cpp
    panels/host.cpp
)

# feature.cpp 的选择控件资源在 device probe 构建下改用严格导出名解析（elf-export:）。
target_compile_definitions(qol_plugin_core.custom_ui PRIVATE
    QOL_ENABLE_DEVICE_PROBE=$<BOOL:${QOL_ENABLE_DEVICE_PROBE}>
    QOL_DEBUG_COMPONENTS=$<IF:$<CONFIG:Debug>,1,0>
)

set_target_properties(qol_plugin_core.custom_ui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
