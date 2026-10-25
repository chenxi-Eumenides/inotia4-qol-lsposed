add_library(qol_plugin_core.custom_ui STATIC
    plugin.cpp
    text_mechanism.cpp
    feature.cpp
    ui_confirmation.cpp
    ui_components.cpp
    ui_shapes.cpp
    settings_contribution.cpp
    panels/stack.cpp
    panels/host.cpp
    panels/frame_overlay.cpp
)
# Debug 面板只在 Debug 构建参与编译：plugin.cpp 中相关引用全部位于 QOL_DEBUG_COMPONENTS
# 条件块内，Release 下没有可达引用。
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_sources(qol_plugin_core.custom_ui PRIVATE
        panels/debug_scroll.cpp
        panels/debug_help.cpp
        panels/debug_components.cpp)
endif()

# 选择控件资源在 device probe 构建下改用严格导出名解析（elf-export:），分支在 plugin.cpp 的 hook:UIChoice_Init 声明。
target_compile_definitions(qol_plugin_core.custom_ui PRIVATE
    QOL_ENABLE_DEVICE_PROBE=$<BOOL:${QOL_ENABLE_DEVICE_PROBE}>
    QOL_DEBUG_COMPONENTS=$<IF:$<CONFIG:Debug>,1,0>
)

set_target_properties(qol_plugin_core.custom_ui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
