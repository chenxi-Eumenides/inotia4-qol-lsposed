# 插件目录只负责发现插件片段、收口公共 target 属性和装配链接关系。
# 每个插件目录通过自己的 CMakeLists.txt 定义 qol_plugin_<id> target。

set(QOL_PLUGIN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

file(GLOB QOL_PLUGIN_CMAKE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt"
)
list(SORT QOL_PLUGIN_CMAKE_FILES)

foreach(plugin_cmake IN LISTS QOL_PLUGIN_CMAKE_FILES)
    get_filename_component(plugin_dir "${plugin_cmake}" DIRECTORY)
    get_filename_component(plugin_id "${plugin_dir}" NAME)
    set(plugin_target "qol_plugin_${plugin_id}")

    if(plugin_id MATCHES "^device_probe_" AND NOT QOL_ENABLE_DEVICE_PROBE)
        continue()
    endif()
    if(QOL_KERNEL_ONLY AND NOT plugin_id STREQUAL "module_health")
        continue()
    endif()

    add_subdirectory("${plugin_dir}")

    if(NOT TARGET "${plugin_target}")
        message(FATAL_ERROR
            "Plugin ${plugin_id} must define target ${plugin_target} in ${plugin_cmake}")
    endif()

    set_target_properties("${plugin_target}" PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories("${plugin_target}" PRIVATE
        "${QOL_PLUGIN_ROOT}/contracts"
        "${plugin_dir}"
    )
    target_include_directories("${plugin_target}" PRIVATE "${QOL_PLUGIN_ROOT}")
    target_compile_features("${plugin_target}" PRIVATE cxx_std_17)
    target_compile_options("${plugin_target}" PRIVATE -Wall -Wextra)
    target_link_libraries(gamebridge PRIVATE "${plugin_target}")
    list(APPEND QOL_PLUGIN_SUNK_IDS "${plugin_id}")
endforeach()

# 根 target 的插件源保持集中清单；拥有独立片段的插件由自己的 target 提供，
# 不再把同一批源文件作为 gamebridge 的直接源重复编译。
set(QOL_BRIDGE_PLUGIN_SOURCES_REMAINING)
foreach(plugin_source IN LISTS QOL_BRIDGE_PLUGIN_SOURCES)
    set(plugin_source_is_sunk FALSE)
    foreach(plugin_id IN LISTS QOL_PLUGIN_SUNK_IDS)
        string(FIND "${plugin_source}" "plugins/${plugin_id}/" plugin_prefix_position)
        if(plugin_prefix_position EQUAL 0)
            set(plugin_source_is_sunk TRUE)
            break()
        endif()
    endforeach()
    if(NOT plugin_source_is_sunk)
        list(APPEND QOL_BRIDGE_PLUGIN_SOURCES_REMAINING "${plugin_source}")
    endif()
endforeach()
set(QOL_BRIDGE_PLUGIN_SOURCES "${QOL_BRIDGE_PLUGIN_SOURCES_REMAINING}" PARENT_SCOPE)
