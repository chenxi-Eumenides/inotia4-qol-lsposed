add_library(qol_plugin_extension_bag STATIC
    extension_bag_state.cpp
    runtime_access.cpp
    runtime_tabs_ui.cpp
    runtime_orchestrator.cpp
    public_runtime.cpp
    runtime_bind.cpp
    api_impl.cpp
    claims.cpp
    move_merge_impl.cpp
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
    api/port_state.cpp
    api/handles.cpp
    api/provider.cpp
    api/api_table.cpp
    api/neutral_provider.cpp
    persistence/persistence.cpp
    extension_bag_geometry.cpp
    extension_bag_api.cpp
    http/routes.cpp
    http_query.cpp
    plugin.cpp
    settings_contribution.cpp
)

target_include_directories(qol_plugin_extension_bag PRIVATE
    "${QOL_PLUGIN_ROOT}/data/./native"
)
