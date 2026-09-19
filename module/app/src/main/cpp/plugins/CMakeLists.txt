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
    target_compile_features("${plugin_target}" PRIVATE cxx_std_17)
    target_compile_options("${plugin_target}" PRIVATE -Wall -Wextra)
    target_link_libraries(gamebridge PRIVATE "${plugin_target}")
endforeach()
