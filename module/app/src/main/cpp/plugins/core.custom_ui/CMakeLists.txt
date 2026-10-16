add_library(qol_plugin_core.custom_ui STATIC
    plugin.cpp
    text_mechanism.cpp
    feature.cpp
    ui_components.cpp
    settings_contribution.cpp
    panels/debug_components.cpp
    panels/stack.cpp
    panels/host.cpp
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_sources(qol_plugin_core.custom_ui PRIVATE panels/debug_scroll.cpp panels/debug_help.cpp)
endif()

# 选择控件资源在 device probe 构建下改用严格导出名解析（elf-export:），分支在 plugin.cpp 的 hook:UIChoice_Init 声明。
target_compile_definitions(qol_plugin_core.custom_ui PRIVATE
    QOL_ENABLE_DEVICE_PROBE=$<BOOL:${QOL_ENABLE_DEVICE_PROBE}>
    QOL_DEBUG_COMPONENTS=$<IF:$<CONFIG:Debug>,1,0>
)

set_target_properties(qol_plugin_core.custom_ui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
