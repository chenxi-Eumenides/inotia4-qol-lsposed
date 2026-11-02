add_library(qol_plugin_custom_recipe STATIC
    model/catalog.cpp
    model/rules.cpp
    table/table.cpp
    table/table_runtime.cpp
    result/feature.cpp
    plugin.cpp
    ui/feature.cpp
    ui/runtime.cpp
)
