add_library(qol_plugin_extension_bag STATIC
    game_ui_virtbag.cpp
    extension_bag_state.cpp
    runtime_observation.cpp
    runtime_tabs.cpp
    runtime_drag_session.cpp
    runtime_input.cpp
    runtime_render.cpp
    runtime_lifecycle.cpp
    ownership.cpp
    transaction.cpp
    ui_equip.cpp
    ui_store.cpp
    ui_mix.cpp
    extension_bag_port.cpp
    extension_bag_persistence.cpp
    extension_bag_geometry.cpp
    extension_bag_api.cpp
    http_query.cpp
    plugin.cpp
    settings_contribution.cpp
)

target_include_directories(qol_plugin_extension_bag PRIVATE
    "${QOL_PLUGIN_ROOT}/data/./native"
)
