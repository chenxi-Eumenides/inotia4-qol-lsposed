add_library(qol_plugin_autosell STATIC
    rules.cpp
    config.cpp
    status_json.cpp
    read_only.cpp
    view.cpp
    scan.cpp
    scan_gates.cpp
    scan_session.cpp
    plugin.cpp
    settings_contribution.cpp
    store.cpp
    ui/panel_ui.cpp
    ui/panel_layout.cpp
    ui/panel_entry.cpp
    ui/panel_components.cpp
    ui/panel_events.cpp
)
