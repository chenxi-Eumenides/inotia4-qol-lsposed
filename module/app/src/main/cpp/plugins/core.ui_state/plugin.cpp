#include "plugin.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "contracts/commands.h"
#include "contracts/game_ports/choice_write_port.h"
#include "contracts/game_ports/ui_write_port.h"
#include "contracts/http.h"
#include "contracts/ui.h"

namespace qol::features::core_ui_state {
namespace {

constexpr const char* kPluginId = "core.ui_state";
constexpr const char* kPluginVersion = "0.1.0";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolUiStateReadPort* g_ui_state_read = nullptr;
uint64_t g_snapshot_task = 0;

constexpr uint32_t kScreenCapacity = 64;
constexpr uint32_t kPanelCapacity = 128;
constexpr uint32_t kDialogCapacity = 8192;
constexpr uint32_t kCustomOptionCount = contracts::kUiStateMaxDialogOptions;

struct UiSnapshotState {
    char screen[kScreenCapacity] = "loading";
    char native_panel[kPanelCapacity] = {};
    char native_dialog[kDialogCapacity] = "{\"error\":\"not in game\"}";
    bool native_snapshot_valid = false;
    uint64_t native_snapshot_epoch = 0;
    uint32_t custom_panel_count = 0;
    char custom_panel_ids[contracts::kUiStateMaxCustomPanels]
                         [contracts::kUiStateMaxPanelIdBytes]{};
    uint64_t custom_panel_sequence = 0;
    bool custom_dialog_active = false;
    bool custom_dialog_has_title = false;
    char custom_dialog_type[contracts::kUiStateMaxDialogTextBytes]{};
    char custom_dialog_title[contracts::kUiStateMaxDialogTextBytes]{};
    char custom_dialog_options[kCustomOptionCount][contracts::kUiStateMaxDialogTextBytes]{};
    uint32_t custom_dialog_option_count = 0;
    uint64_t custom_dialog_generation = 0;
    uint64_t custom_dialog_v1_sequence = 0;
    uint64_t custom_dialog_last_generation = 0;
    uint64_t custom_dialog_sequence = 0;
};

std::mutex g_snapshot_mutex;
UiSnapshotState g_snapshot{};

const contracts::CapabilityRequirement kOptionalHttpRequires[] = {
    {
        contracts::kHttpRegistryCapabilityId,
        {contracts::kHttpRegistryVersion.major, contracts::kHttpRegistryVersion.minor},
    },
    {
        contracts::kHttpDispatchCapabilityId,
        {contracts::kHttpDispatchVersion.major, contracts::kHttpDispatchVersion.minor},
    },
};

// 选择控件（能力 + 2 条资源 + QOL_PORT_CHOICE）归 core.custom_ui；本 owner 保留 UI 写端口
// 与主线程 UI 状态读端口。
const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CHOICE_WRITE,
     {contracts::kQolPortChoiceWriteVersion.major, contracts::kQolPortChoiceWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_UI_WRITE,
     {contracts::kQolPortUiWriteVersion.major, contracts::kQolPortUiWriteVersion.minor}},
    {contracts::QolPortId::QOL_PORT_UI_STATE_READ,
     {contracts::kQolPortUiStateReadVersion.major,
      contracts::kQolPortUiStateReadVersion.minor}},
};

bool main_thread_now() {
    if (g_sdk == nullptr || g_sdk->ui == nullptr || g_sdk->ui->is_main_thread == nullptr) {
        return false;
    }
    return g_sdk->ui->is_main_thread() != 0;
}

QolResult state_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

bool copy_string(char* out, uint32_t capacity, const char* value) {
    if (out == nullptr || capacity == 0 || value == nullptr) return false;
    const size_t length = std::strlen(value);
    if (length >= capacity) return false;
    if (length > 0) std::memcpy(out, value, length);
    out[length] = '\0';
    return true;
}

bool valid_type_id(const char* value) {
    if (value == nullptr || value[0] == '\0') return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p != 0; ++p) {
        const unsigned char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

bool valid_utf8(const char* value, size_t length) {
    if (value == nullptr) return false;
    size_t i = 0;
    while (i < length) {
        const uint8_t first = static_cast<uint8_t>(value[i]);
        if (first <= 0x7f) {
            ++i;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            if (i + 1 >= length) return false;
            const uint8_t second = static_cast<uint8_t>(value[i + 1]);
            if (second < 0x80 || second > 0xbf) return false;
            i += 2;
            continue;
        }
        if (first >= 0xe0 && first <= 0xef) {
            if (i + 2 >= length) return false;
            const uint8_t second = static_cast<uint8_t>(value[i + 1]);
            const uint8_t third = static_cast<uint8_t>(value[i + 2]);
            const bool valid_second = first == 0xe0 ? second >= 0xa0 && second <= 0xbf
                                      : first == 0xed ? second >= 0x80 && second <= 0x9f
                                                      : second >= 0x80 && second <= 0xbf;
            if (!valid_second || third < 0x80 || third > 0xbf) return false;
            i += 3;
            continue;
        }
        if (first >= 0xf0 && first <= 0xf4) {
            if (i + 3 >= length) return false;
            const uint8_t second = static_cast<uint8_t>(value[i + 1]);
            const uint8_t third = static_cast<uint8_t>(value[i + 2]);
            const uint8_t fourth = static_cast<uint8_t>(value[i + 3]);
            const bool valid_second = first == 0xf0 ? second >= 0x90 && second <= 0xbf
                                      : first == 0xf4 ? second >= 0x80 && second <= 0x8f
                                                      : second >= 0x80 && second <= 0xbf;
            if (!valid_second || third < 0x80 || third > 0xbf ||
                fourth < 0x80 || fourth > 0xbf) return false;
            i += 4;
            continue;
        }
        return false;
    }
    return true;
}

bool valid_fixed_utf8(const char* value, size_t capacity) {
    if (value == nullptr || capacity == 0) return false;
    const void* terminator = std::memchr(value, '\0', capacity);
    if (terminator == nullptr) return false;
    const size_t length = static_cast<const char*>(terminator) - value;
    return valid_utf8(value, length);
}

QolResult publish_custom_panels(const contracts::UiCustomPanelsSnapshot* snapshot) {
    if (snapshot == nullptr || snapshot->sequence == 0 ||
        snapshot->count > contracts::kUiStateMaxCustomPanels ||
        (snapshot->count > 0 && snapshot->panel_ids == nullptr)) {
        return state_error(kQolErrInvalidArgument, "invalid custom panel snapshot");
    }
    if (!main_thread_now()) return state_error(kQolErrBlocked, "wrong thread");

    char copied[contracts::kUiStateMaxCustomPanels][contracts::kUiStateMaxPanelIdBytes]{};
    for (uint32_t i = 0; i < snapshot->count; ++i) {
        if (!copy_string(copied[i], sizeof(copied[i]), snapshot->panel_ids[i]) ||
            copied[i][0] == '\0') {
            return state_error(kQolErrInvalidArgument, "invalid custom panel id");
        }
    }

    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    if (snapshot->sequence <= g_snapshot.custom_panel_sequence) {
        return state_error(kQolErrConflict, "stale custom panel snapshot");
    }
    g_snapshot.custom_panel_sequence = snapshot->sequence;
    g_snapshot.custom_panel_count = snapshot->count;
    for (uint32_t i = 0; i < snapshot->count; ++i) {
        std::memcpy(g_snapshot.custom_panel_ids[i], copied[i], sizeof(copied[i]));
    }
    for (uint32_t i = snapshot->count; i < contracts::kUiStateMaxCustomPanels; ++i) {
        g_snapshot.custom_panel_ids[i][0] = '\0';
    }
    return contracts::ok();
}

QolResult publish_custom_dialog(const contracts::UiCustomDialogSnapshot* snapshot) {
    if (snapshot == nullptr || snapshot->sequence == 0 || snapshot->generation == 0 ||
        !valid_type_id(snapshot->type_id) ||
        snapshot->option_count > kCustomOptionCount ||
        (snapshot->option_count > 0 && snapshot->option_labels == nullptr)) {
        return state_error(kQolErrInvalidArgument, "invalid custom dialog snapshot");
    }
    if (!main_thread_now()) return state_error(kQolErrBlocked, "wrong thread");

    char type_id[contracts::kUiStateMaxDialogTextBytes]{};
    char title[contracts::kUiStateMaxDialogTextBytes]{};
    char options[kCustomOptionCount][contracts::kUiStateMaxDialogTextBytes]{};
    if (!copy_string(type_id, sizeof(type_id), snapshot->type_id)) {
        return state_error(kQolErrInvalidArgument, "custom dialog type too long");
    }
    const bool has_title = snapshot->title != nullptr;
    if (has_title && !copy_string(title, sizeof(title), snapshot->title)) {
        return state_error(kQolErrInvalidArgument, "custom dialog title too long");
    }
    for (uint32_t i = 0; i < snapshot->option_count; ++i) {
        if (!copy_string(options[i], sizeof(options[i]), snapshot->option_labels[i])) {
            return state_error(kQolErrInvalidArgument, "custom dialog option too long");
        }
    }

    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    if (snapshot->sequence <= g_snapshot.custom_dialog_sequence ||
        snapshot->generation <= g_snapshot.custom_dialog_last_generation) {
        return state_error(kQolErrConflict, "stale custom dialog snapshot");
    }
    g_snapshot.custom_dialog_sequence = snapshot->sequence;
    g_snapshot.custom_dialog_last_generation = snapshot->generation;
    g_snapshot.custom_dialog_generation = snapshot->generation;
    g_snapshot.custom_dialog_active = true;
    g_snapshot.custom_dialog_v1_sequence = snapshot->sequence;
    g_snapshot.custom_dialog_has_title = has_title;
    g_snapshot.custom_dialog_option_count = snapshot->option_count;
    std::memcpy(g_snapshot.custom_dialog_type, type_id, sizeof(type_id));
    std::memcpy(g_snapshot.custom_dialog_title, title, sizeof(title));
    for (uint32_t i = 0; i < kCustomOptionCount; ++i) {
        if (i < snapshot->option_count) {
            std::memcpy(g_snapshot.custom_dialog_options[i], options[i], sizeof(options[i]));
        } else {
            g_snapshot.custom_dialog_options[i][0] = '\0';
        }
    }
    return contracts::ok();
}

QolResult clear_custom_dialog(uint64_t generation, uint64_t sequence) {
    if (generation == 0 || sequence == 0) {
        return state_error(kQolErrInvalidArgument, "invalid custom dialog generation");
    }
    if (!main_thread_now()) return state_error(kQolErrBlocked, "wrong thread");
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    if (sequence <= g_snapshot.custom_dialog_sequence) {
        return state_error(kQolErrConflict, "stale custom dialog clear");
    }
    g_snapshot.custom_dialog_sequence = sequence;
    if (generation > g_snapshot.custom_dialog_last_generation) {
        g_snapshot.custom_dialog_last_generation = generation;
    }
    if (g_snapshot.custom_dialog_active && g_snapshot.custom_dialog_generation == generation) {
        g_snapshot.custom_dialog_active = false;
        g_snapshot.custom_dialog_has_title = false;
        g_snapshot.custom_dialog_option_count = 0;
        g_snapshot.custom_dialog_v1_sequence = 0;
    }
    return contracts::ok();
}

QolResult read_layers(contracts::QolUiLayerVisitFn visit, void* user);
QolResult read_state(contracts::QolUiStateSummary* out);
QolResult visit_states(contracts::QolUiStateTableVisitFn visit, void* user);
QolResult register_state(const contracts::QolUiStateRegistration* registration);
QolResult unregister_state(uint32_t type, const char* key);
QolResult publish_external_ui(const contracts::QolUiExternalSnapshot* snapshot);

const contracts::UiStateApi kUiStateApi{
    &publish_custom_panels,
    &publish_custom_dialog,
    &clear_custom_dialog,
    &read_layers,
    &read_state,
    &visit_states,
    &register_state,
    &unregister_state,
    &publish_external_ui,
};

const contracts::CapabilityProvisionRef kProvides[] = {
    {contracts::kUiStateCapabilityId,
     {contracts::kUiStateVersionMajor, contracts::kUiStateVersionMinor}, &kUiStateApi},
};

int32_t native_snapshot_tick(int64_t, void*) {
    auto invalidate_snapshot = [] {
        std::lock_guard<std::mutex> lock(g_snapshot_mutex);
        g_snapshot.native_snapshot_valid = false;
        ++g_snapshot.native_snapshot_epoch;
    };
    if (g_ui_state_read == nullptr || g_ui_state_read->read.read_snapshot == nullptr) {
        invalidate_snapshot();
        return 1;
    }
    char screen[kScreenCapacity]{};
    char panel[kPanelCapacity]{};
    char dialog[kDialogCapacity]{};
    uint32_t screen_size = 0;
    uint32_t panel_size = 0;
    uint32_t dialog_size = 0;
    const QolResult result = g_ui_state_read->read.read_snapshot(
        screen, sizeof(screen), &screen_size, panel, sizeof(panel), &panel_size,
        dialog, sizeof(dialog), &dialog_size);
    if (!contracts::succeeded(result) || screen_size >= sizeof(screen) ||
        panel_size >= sizeof(panel) || dialog_size >= sizeof(dialog) ||
        screen[screen_size] != '\0' || panel[panel_size] != '\0' || dialog[dialog_size] != '\0') {
        invalidate_snapshot();
        return 1;
    }
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    std::memcpy(g_snapshot.screen, screen, screen_size + 1);
    std::memcpy(g_snapshot.native_panel, panel, panel_size + 1);
    std::memcpy(g_snapshot.native_dialog, dialog, dialog_size + 1);
    g_snapshot.native_snapshot_valid = true;
    ++g_snapshot.native_snapshot_epoch;
    return 1;
}

struct UiSnapshotCopy {
    char screen[kScreenCapacity]{};
    char native_panel[kPanelCapacity]{};
    char native_dialog[kDialogCapacity]{};
    bool native_snapshot_valid = false;
    uint32_t custom_panel_count = 0;
    char custom_panel_ids[contracts::kUiStateMaxCustomPanels]
                         [contracts::kUiStateMaxPanelIdBytes]{};
    bool custom_dialog_active = false;
    bool custom_dialog_has_title = false;
    char custom_dialog_type[contracts::kUiStateMaxDialogTextBytes]{};
    char custom_dialog_title[contracts::kUiStateMaxDialogTextBytes]{};
    char custom_dialog_options[kCustomOptionCount][contracts::kUiStateMaxDialogTextBytes]{};
    uint32_t custom_dialog_option_count = 0;
    uint64_t custom_dialog_v1_sequence = 0;
};

UiSnapshotCopy copy_ui_snapshot() {
    UiSnapshotCopy copy{};
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    std::memcpy(copy.screen, g_snapshot.screen, sizeof(copy.screen));
    std::memcpy(copy.native_panel, g_snapshot.native_panel, sizeof(copy.native_panel));
    std::memcpy(copy.native_dialog, g_snapshot.native_dialog, sizeof(copy.native_dialog));
    copy.native_snapshot_valid = g_snapshot.native_snapshot_valid;
    copy.custom_panel_count = g_snapshot.custom_panel_count;
    std::memcpy(copy.custom_panel_ids, g_snapshot.custom_panel_ids,
                sizeof(copy.custom_panel_ids));
    copy.custom_dialog_active = g_snapshot.custom_dialog_active;
    copy.custom_dialog_has_title = g_snapshot.custom_dialog_has_title;
    std::memcpy(copy.custom_dialog_type, g_snapshot.custom_dialog_type,
                sizeof(copy.custom_dialog_type));
    std::memcpy(copy.custom_dialog_title, g_snapshot.custom_dialog_title,
                sizeof(copy.custom_dialog_title));
    std::memcpy(copy.custom_dialog_options, g_snapshot.custom_dialog_options,
                sizeof(copy.custom_dialog_options));
    copy.custom_dialog_option_count = g_snapshot.custom_dialog_option_count;
    copy.custom_dialog_v1_sequence = g_snapshot.custom_dialog_v1_sequence;
    return copy;
}

// 从 dialog JSON 取顶层 "type" 字段；找不到或过长返回 false。
bool json_type_field(const char* json, char* out, uint32_t capacity) {
    if (json == nullptr || out == nullptr || capacity == 0) return false;
    static constexpr char kKey[] = "\"type\":\"";
    const char* key = std::strstr(json, kKey);
    if (key == nullptr) return false;
    key += sizeof(kKey) - 1;
    const char* end = std::strchr(key, '"');
    if (end == nullptr) return false;
    const size_t length = static_cast<size_t>(end - key);
    if (length == 0 || length >= capacity) return false;
    std::memcpy(out, key, length);
    out[length] = '\0';
    return true;
}

QolResult read_layers(contracts::QolUiLayerVisitFn visit, void* user) {
    if (visit == nullptr) return state_error(kQolErrInvalidArgument, "invalid layer visitor");
    const UiSnapshotCopy snapshot = copy_ui_snapshot();
    auto emit = [visit, user](const char* id, contracts::UiLayerType type) {
        if (id == nullptr || id[0] == '\0') return;
        const contracts::UiLayerInfo layer{id, static_cast<uint32_t>(type)};
        visit(&layer, user);
    };
    // 栈顶 → 栈底：自定义对话框 → 自定义面板（后压入者在上）→ 原生对话框 → 原生面板 → screen。
    if (snapshot.custom_dialog_active) {
        emit(snapshot.custom_dialog_type, contracts::UiLayerType::kDialog);
    }
    for (uint32_t i = snapshot.custom_panel_count; i > 0; --i) {
        emit(snapshot.custom_panel_ids[i - 1], contracts::UiLayerType::kPanel);
    }
    char dialog_type[contracts::kUiStateMaxDialogTextBytes]{};
    if (snapshot.native_snapshot_valid &&
        json_type_field(snapshot.native_dialog, dialog_type, sizeof(dialog_type)) &&
        std::strcmp(dialog_type, "none") != 0) {
        emit(dialog_type, contracts::UiLayerType::kDialog);
    }
    emit(snapshot.native_panel, contracts::UiLayerType::kPanel);
    emit(snapshot.screen, contracts::UiLayerType::kOther);
    return contracts::ok();
}

std::string json_quote(const char* value) {
    std::string out = "\"";
    if (value != nullptr) {
        static constexpr char hex[] = "0123456789abcdef";
        for (const unsigned char c : std::string(value)) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        out += "\\u00";
                        out += hex[c >> 4];
                        out += hex[c & 0x0f];
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
    }
    out += '"';
    return out;
}

// ---- ui.state 1.2：统一状态 ----

struct NativeStateRow {
    const char* screen;
    const char* name;
    uint32_t type;
};

constexpr NativeStateRow kNativeStateRows[] = {
    {"loading", "loading", contracts::kUiStateTypeBoot},
    {"main_menu", "main_menu", contracts::kUiStateTypeMenu},
    {"world", "world", contracts::kUiStateTypeWorld},
    {"tutorial_pause", "tutorial_pause", contracts::kUiStateTypeWorld},
    {"dialog_popup", "game_popup", contracts::kUiStateTypeDialog},
    {"dialog_story", "game_story", contracts::kUiStateTypeDialog},
    {"dialog_wipeout", "game_wipeout", contracts::kUiStateTypeDialog},
    {"dialog_quest", "game_npc_quest", contracts::kUiStateTypeDialog},
    {"dialog_npc", "game_npc_dialog", contracts::kUiStateTypeDialog},
    {"dialog_choice", "game_choice", contracts::kUiStateTypeDialog},
    {"dialog_input_count", "game_input_count", contracts::kUiStateTypeDialog},
    {"panel_character_info", "panel_character_info", contracts::kUiStateTypePanel},
    {"panel_inventory", "panel_inventory", contracts::kUiStateTypePanel},
    {"panel_skills", "panel_skills", contracts::kUiStateTypePanel},
    {"panel_mercenary", "panel_mercenary", contracts::kUiStateTypePanel},
    {"panel_quests", "panel_quests", contracts::kUiStateTypePanel},
    {"panel_settings", "panel_settings", contracts::kUiStateTypePanel},
    {"panel_shop", "panel_shop", contracts::kUiStateTypePanel},
    {"panel_craft", "panel_craft", contracts::kUiStateTypePanel},
    {"panel_npc_rest", "panel_npc_rest", contracts::kUiStateTypePanel},
    {"panel_npc_revive", "panel_npc_revive", contracts::kUiStateTypePanel},
    {"panel_save_slot", "panel_save_slot", contracts::kUiStateTypePanel},
    {"panel_character_select", "panel_character_select", contracts::kUiStateTypePanel},
    {"panel_options", "panel_options", contracts::kUiStateTypePanel},
    {"panel_shortcut", "panel_shortcut", contracts::kUiStateTypePanel},
    {"panel_world_map", "panel_world_map", contracts::kUiStateTypePanel},
    {"panel_daily_reward", "panel_daily_reward", contracts::kUiStateTypePanel},
    {"panel_in_app", "panel_in_app", contracts::kUiStateTypePanel},
    {"panel_ui_panel", "panel_ui_panel", contracts::kUiStateTypePanel},
    {"main_menu_save_slot", "main_menu_save_slot", contracts::kUiStateTypePanel},
    {"main_menu_character_select", "main_menu_character_select", contracts::kUiStateTypePanel},
    {"main_menu_daily_reward", "main_menu_daily_reward", contracts::kUiStateTypePanel},
    {"main_menu_options", "main_menu_options", contracts::kUiStateTypePanel},
    {"main_menu_settings", "main_menu_settings", contracts::kUiStateTypePanel},
};

const NativeStateRow* native_row_for_screen(const char* screen) {
    if (screen == nullptr) return nullptr;
    for (const auto& row : kNativeStateRows) {
        if (std::strcmp(row.screen, screen) == 0) return &row;
    }
    return nullptr;
}

bool module_state_name(uint32_t type, const char* key, char* out, uint32_t capacity) {
    if (out == nullptr || capacity == 0 || key == nullptr || key[0] == '\0') return false;
    const char* prefix = nullptr;
    if (type == contracts::kUiStateTypeDialog) {
        prefix = "module_dialog_";
    } else if (type == contracts::kUiStateTypePanel) {
        prefix = "module_panel_";
    } else {
        return false;
    }
    size_t written = std::strlen(prefix);
    if (written >= capacity) return false;
    std::memcpy(out, prefix, written);
    for (const char* p = key; *p != '\0'; ++p) {
        char c = *p;
        if (c == '.') c = '_';
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!valid || written + 1 >= capacity) return false;
        out[written++] = c;
    }
    out[written] = '\0';
    return true;
}

struct RegisteredState {
    uint32_t type = 0;
    char key[64] = {};
    char plugin_id[64] = {};
    char name[contracts::kUiStateMaxNameBytes] = {};
};

constexpr uint32_t kMaxRegisteredStates = 32;
RegisteredState g_registered[kMaxRegisteredStates];
uint32_t g_registered_count = 0;
std::mutex g_registered_mutex;

struct SeenState {
    char name[contracts::kUiStateMaxNameBytes] = {};
    uint32_t type = 0;
    uint32_t source = 0;
};

constexpr uint32_t kMaxSeenStates = 32;
SeenState g_seen[kMaxSeenStates];
uint32_t g_seen_count = 0;
std::mutex g_seen_mutex;

std::mutex g_external_mutex;
bool g_external_active = false;
char g_external_name[contracts::kUiStateMaxNameBytes] = "android_agreement";

void mark_seen(const char* name, uint32_t type, uint32_t source) {
    if (name == nullptr || name[0] == '\0') return;
    std::lock_guard<std::mutex> lock(g_seen_mutex);
    for (uint32_t i = 0; i < g_seen_count; ++i) {
        if (std::strcmp(g_seen[i].name, name) == 0) return;
    }
    if (g_seen_count >= kMaxSeenStates) return;
    std::snprintf(g_seen[g_seen_count].name, sizeof(g_seen[g_seen_count].name), "%s", name);
    g_seen[g_seen_count].type = type;
    g_seen[g_seen_count].source = source;
    ++g_seen_count;
}

bool external_overlay_name(char* out, uint32_t capacity) {
    if (out == nullptr || capacity == 0) return false;
    std::lock_guard<std::mutex> lock(g_external_mutex);
    if (!g_external_active || g_external_name[0] == '\0') return false;
    std::snprintf(out, capacity, "%s", g_external_name);
    return true;
}

struct ResolvedState {
    std::string name;
    uint32_t type;
    uint32_t source;
};

ResolvedState resolve_top(const UiSnapshotCopy& snapshot) {
    char external[contracts::kUiStateMaxNameBytes]{};
    if (external_overlay_name(external, sizeof(external))) {
        return {external, contracts::kUiStateTypeDialog, contracts::kUiStateSourceAndroid};
    }
    if (!snapshot.native_snapshot_valid) {
        return {"unknown", contracts::kUiStateTypeUnknown, contracts::kUiStateSourceUnknown};
    }
    if (std::strcmp(snapshot.screen, "dialog_popup") == 0) {
        return {"game_popup", contracts::kUiStateTypeDialog, contracts::kUiStateSourceNative};
    }
    if (snapshot.custom_dialog_active) {
        char name[contracts::kUiStateMaxNameBytes]{};
        if (!module_state_name(contracts::kUiStateTypeDialog, snapshot.custom_dialog_type, name,
                               sizeof(name))) {
            std::snprintf(name, sizeof(name), "%s", "module_dialog_other");
        }
        mark_seen(name, contracts::kUiStateTypeDialog, contracts::kUiStateSourceModule);
        return {name, contracts::kUiStateTypeDialog, contracts::kUiStateSourceModule};
    }
    const NativeStateRow* native = native_row_for_screen(snapshot.screen);
    if (native != nullptr && native->type == contracts::kUiStateTypeDialog) {
        return {native->name, native->type, contracts::kUiStateSourceNative};
    }
    if (snapshot.custom_panel_count > 0) {
        const char* id = snapshot.custom_panel_ids[snapshot.custom_panel_count - 1];
        char name[contracts::kUiStateMaxNameBytes]{};
        if (!module_state_name(contracts::kUiStateTypePanel, id, name, sizeof(name))) {
            std::snprintf(name, sizeof(name), "%s", "module_panel_other");
        }
        mark_seen(name, contracts::kUiStateTypePanel, contracts::kUiStateSourceModule);
        return {name, contracts::kUiStateTypePanel, contracts::kUiStateSourceModule};
    }
    if (native != nullptr) {
        return {native->name, native->type, contracts::kUiStateSourceNative};
    }
    return {"unknown", contracts::kUiStateTypeUnknown, contracts::kUiStateSourceUnknown};
}

// 取顶层 JSON 字段的原文（字符串含引号，数组含方括号），用于原样嵌入统一响应。
bool raw_json_field(const char* json, const char* key, std::string* out) {
    if (json == nullptr || key == nullptr || out == nullptr) return false;
    const std::string pattern = std::string("\"") + key + "\"";
    const char* p = std::strstr(json, pattern.c_str());
    if (p == nullptr) return false;
    p += pattern.size();
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != ':') return false;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    const char* begin = p;
    if (*p == '"') {
        ++p;
        bool closed = false;
        while (*p != '\0') {
            if (*p == '\\') {
                if (p[1] == '\0') return false;
                p += 2;
                continue;
            }
            if (*p == '"') {
                ++p;
                closed = true;
                break;
            }
            ++p;
        }
        if (!closed) return false;
    } else if (*p == '[') {
        int depth = 0;
        bool in_string = false;
        bool closed = false;
        for (; *p != '\0'; ++p) {
            const char c = *p;
            if (in_string) {
                if (c == '\\') {
                    if (p[1] == '\0') return false;
                    ++p;
                    continue;
                }
                if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '[') {
                ++depth;
            } else if (c == ']') {
                --depth;
                if (depth == 0) {
                    ++p;
                    closed = true;
                    break;
                }
            }
        }
        if (!closed) return false;
    } else {
        while (*p != '\0' && *p != ',' && *p != '}') ++p;
    }
    out->assign(begin, static_cast<size_t>(p - begin));
    return !out->empty();
}

std::string build_info_json(const UiSnapshotCopy& snapshot, const ResolvedState& top) {
    std::string title;
    std::string content;
    bool has_title = false;
    bool has_content = false;
    if (top.source == contracts::kUiStateSourceNative &&
        top.type == contracts::kUiStateTypeDialog) {
        has_title = raw_json_field(snapshot.native_dialog, "speaker", &title);
        has_content = raw_json_field(snapshot.native_dialog, "text", &content);
    } else if (top.source == contracts::kUiStateSourceModule &&
               top.type == contracts::kUiStateTypeDialog) {
        if (snapshot.custom_dialog_has_title && snapshot.custom_dialog_title[0] != '\0') {
            title = json_quote(snapshot.custom_dialog_title);
            has_title = true;
        }
        has_content = raw_json_field(snapshot.native_dialog, "text", &content);
    }
    std::string info = "{";
    bool first = true;
    const auto append = [&info, &first](const char* key, const std::string& value) {
        if (!first) info += ",";
        first = false;
        info += json_quote(key);
        info += ":";
        info += value;
    };
    if (has_title) append("title", title);
    if (has_content) append("content", content);
    info += "}";
    return info;
}

std::string build_options_json(const UiSnapshotCopy& snapshot, const ResolvedState& top) {
    if (top.source == contracts::kUiStateSourceAndroid) {
        return "[{\"id\":\"ok\",\"label\":\"同意\"}]";
    }
    if (top.source == contracts::kUiStateSourceModule &&
        top.type == contracts::kUiStateTypePanel) {
        return "[]";
    }
    if ((top.source == contracts::kUiStateSourceNative &&
         (top.type == contracts::kUiStateTypeDialog || top.type == contracts::kUiStateTypePanel)) ||
        (top.source == contracts::kUiStateSourceModule &&
         top.type == contracts::kUiStateTypeDialog)) {
        std::string options;
        if (raw_json_field(snapshot.native_dialog, "options", &options)) return options;
        if (top.source == contracts::kUiStateSourceModule &&
            top.type == contracts::kUiStateTypeDialog &&
            snapshot.custom_dialog_option_count > 0) {
            std::string built = "[";
            for (uint32_t i = 0; i < snapshot.custom_dialog_option_count; ++i) {
                if (i > 0) built += ",";
                built += "{\"id\":\"";
                built += std::to_string(i);
                built += "\",\"label\":";
                built += json_quote(snapshot.custom_dialog_options[i]);
                built += "}";
            }
            built += "]";
            return built;
        }
    }
    return "[]";
}

QolResult read_state(contracts::QolUiStateSummary* out) {
    if (out == nullptr) return state_error(kQolErrInvalidArgument, "state output required");
    if (out->struct_size != 0 && out->struct_size < sizeof(*out)) {
        return state_error(kQolErrInvalidArgument, "state output too small");
    }
    const UiSnapshotCopy snapshot = copy_ui_snapshot();
    const ResolvedState top = resolve_top(snapshot);
    out->struct_size = sizeof(*out);
    out->type = top.type;
    out->source = top.source;
    std::snprintf(out->name, sizeof(out->name), "%s", top.name.c_str());
    return contracts::ok();
}

struct TableEntry {
    std::string name;
    uint32_t type;
    uint32_t source;
    uint32_t stability;
};

void add_table_entry(std::vector<TableEntry>* rows, const char* name, uint32_t type,
                     uint32_t source, uint32_t stability) {
    if (rows == nullptr || name == nullptr || name[0] == '\0') return;
    for (const auto& row : *rows) {
        if (row.name == name) return;
    }
    rows->push_back(TableEntry{name, type, source, stability});
}

void collect_table(std::vector<TableEntry>* rows) {
    add_table_entry(rows, "unknown", contracts::kUiStateTypeUnknown,
                    contracts::kUiStateSourceUnknown, 0);
    for (const auto& row : kNativeStateRows) {
        add_table_entry(rows, row.name, row.type, contracts::kUiStateSourceNative, 0);
    }
    if (g_sdk != nullptr && g_sdk->panel_enumeration != nullptr &&
        g_sdk->panel_enumeration->count != nullptr && g_sdk->panel_enumeration->at != nullptr) {
        const uint32_t provider_count = g_sdk->panel_enumeration->count();
        for (uint32_t i = 0; i < provider_count; ++i) {
            const contracts::PanelProviderSpec* provider = g_sdk->panel_enumeration->at(i);
            if (provider == nullptr || provider->panel_id == nullptr ||
                provider->panel_id[0] == '\0') {
                continue;
            }
            char name[contracts::kUiStateMaxNameBytes]{};
            if (!module_state_name(contracts::kUiStateTypePanel, provider->panel_id, name,
                                   sizeof(name))) {
                continue;
            }
            add_table_entry(rows, name, contracts::kUiStateTypePanel,
                            contracts::kUiStateSourceModule, 0);
        }
    }
    add_table_entry(rows, "android_agreement", contracts::kUiStateTypeDialog,
                    contracts::kUiStateSourceAndroid, 0);
    {
        std::lock_guard<std::mutex> lock(g_registered_mutex);
        for (uint32_t i = 0; i < g_registered_count; ++i) {
            add_table_entry(rows, g_registered[i].name, g_registered[i].type,
                            contracts::kUiStateSourceModule, 0);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_seen_mutex);
        for (uint32_t i = 0; i < g_seen_count; ++i) {
            add_table_entry(rows, g_seen[i].name, g_seen[i].type, g_seen[i].source, 0);
        }
    }
    add_table_entry(rows, "module_panel_other", contracts::kUiStateTypePanel,
                    contracts::kUiStateSourceModule, 0);
    add_table_entry(rows, "module_dialog_other", contracts::kUiStateTypeDialog,
                    contracts::kUiStateSourceModule, 0);
}

QolResult visit_states(contracts::QolUiStateTableVisitFn visit, void* user) {
    if (visit == nullptr) return state_error(kQolErrInvalidArgument, "state visitor required");
    std::vector<TableEntry> rows;
    collect_table(&rows);
    for (const auto& entry : rows) {
        const contracts::QolUiStateTableRow row{entry.name.c_str(), entry.type, entry.source,
                                                entry.stability};
        visit(&row, user);
    }
    return contracts::ok();
}

QolResult register_state(const contracts::QolUiStateRegistration* registration) {
    if (registration == nullptr || registration->struct_size < sizeof(*registration) ||
        (registration->type != contracts::kUiStateTypeDialog &&
         registration->type != contracts::kUiStateTypePanel)) {
        return state_error(kQolErrInvalidArgument, "invalid state registration");
    }
    char key[64]{};
    char plugin_id[64]{};
    if (!copy_string(key, sizeof(key), registration->key) || key[0] == '\0' ||
        !copy_string(plugin_id, sizeof(plugin_id), registration->plugin_id) ||
        plugin_id[0] == '\0') {
        return state_error(kQolErrInvalidArgument, "state key and plugin id required");
    }
    char name[contracts::kUiStateMaxNameBytes]{};
    if (!module_state_name(registration->type, key, name, sizeof(name))) {
        return state_error(kQolErrInvalidArgument, "invalid state key");
    }
    std::lock_guard<std::mutex> lock(g_registered_mutex);
    for (uint32_t i = 0; i < g_registered_count; ++i) {
        if (std::strcmp(g_registered[i].name, name) == 0) {
            return state_error(kQolErrConflict, "state name already registered");
        }
    }
    if (g_registered_count >= kMaxRegisteredStates) {
        return state_error(kQolErrInternal, "state registry full");
    }
    RegisteredState& entry = g_registered[g_registered_count++];
    entry.type = registration->type;
    std::memcpy(entry.key, key, sizeof(entry.key));
    std::memcpy(entry.plugin_id, plugin_id, sizeof(entry.plugin_id));
    std::memcpy(entry.name, name, sizeof(entry.name));
    return contracts::ok();
}

QolResult unregister_state(uint32_t type, const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return state_error(kQolErrInvalidArgument, "state key required");
    }
    char name[contracts::kUiStateMaxNameBytes]{};
    if (!module_state_name(type, key, name, sizeof(name))) return contracts::ok();
    std::lock_guard<std::mutex> lock(g_registered_mutex);
    for (uint32_t i = 0; i < g_registered_count; ++i) {
        if (std::strcmp(g_registered[i].name, name) != 0) continue;
        for (uint32_t j = i + 1; j < g_registered_count; ++j) {
            g_registered[j - 1] = g_registered[j];
        }
        --g_registered_count;
        g_registered[g_registered_count] = RegisteredState{};
        break;
    }
    return contracts::ok();
}

QolResult publish_external_ui(const contracts::QolUiExternalSnapshot* snapshot) {
    if (snapshot == nullptr || snapshot->struct_size < sizeof(*snapshot)) {
        return state_error(kQolErrInvalidArgument, "invalid external ui snapshot");
    }
    std::lock_guard<std::mutex> lock(g_external_mutex);
    g_external_active = snapshot->active != 0;
    if (g_external_active) {
        if (snapshot->name[0] == '\0' ||
            std::memchr(snapshot->name, '\0', sizeof(snapshot->name)) == nullptr) {
            g_external_active = false;
            return state_error(kQolErrInvalidArgument, "external ui name required");
        }
        std::snprintf(g_external_name, sizeof(g_external_name), "%s", snapshot->name);
    } else {
        g_external_name[0] = '\0';
    }
    return contracts::ok();
}

class JsonScanner {
public:
    explicit JsonScanner(const std::string& input) : input_(input) {}

    bool replace_top_level_type(const char* replacement, std::string* output) {
        if (replacement == nullptr || output == nullptr) return false;
        skip_whitespace();
        if (!consume('{')) return false;
        skip_whitespace();
        bool found_type = false;
        size_t type_begin = 0;
        size_t type_end = 0;
        if (consume('}')) return false;
        for (;;) {
            std::string key;
            if (!parse_string(&key)) return false;
            skip_whitespace();
            if (!consume(':')) return false;
            skip_whitespace();
            if (key == "type") {
                if (found_type || position_ >= input_.size() || input_[position_] != '"') {
                    return false;
                }
                found_type = true;
                type_begin = position_;
                if (!parse_string(nullptr)) return false;
                type_end = position_;
            } else if (!parse_value(1)) {
                return false;
            }
            skip_whitespace();
            if (consume('}')) break;
            if (!consume(',')) return false;
            skip_whitespace();
        }
        skip_whitespace();
        if (!found_type || position_ != input_.size()) return false;
        *output = input_.substr(0, type_begin) + json_quote(replacement) +
                  input_.substr(type_end);
        return true;
    }

private:
    static constexpr unsigned int kMaxDepth = 64;

    void skip_whitespace() {
        while (position_ < input_.size()) {
            const char c = input_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool parse_string(std::string* decoded) {
        if (!consume('"')) return false;
        if (decoded != nullptr) decoded->clear();
        while (position_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[position_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                if (decoded != nullptr) decoded->push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
            if (escaped == 'u') {
                if (input_.size() - position_ < 4) return false;
                unsigned int codepoint = 0;
                for (int i = 0; i < 4; ++i) {
                    const char digit = input_[position_++];
                    codepoint <<= 4;
                    if (digit >= '0' && digit <= '9') codepoint |= digit - '0';
                    else if (digit >= 'a' && digit <= 'f') codepoint |= digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F') codepoint |= digit - 'A' + 10;
                    else return false;
                }
                if (decoded != nullptr) {
                    decoded->push_back(codepoint <= 0x7f ? static_cast<char>(codepoint) : '\x7f');
                }
                continue;
            }
            char value = '\0';
            switch (escaped) {
                case '"': value = '"'; break;
                case '\\': value = '\\'; break;
                case '/': value = '/'; break;
                case 'b': value = '\b'; break;
                case 'f': value = '\f'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                default: return false;
            }
            if (decoded != nullptr) decoded->push_back(value);
        }
        return false;
    }

    bool parse_value(unsigned int depth) {
        if (depth > kMaxDepth) return false;
        skip_whitespace();
        if (position_ >= input_.size()) return false;
        const char c = input_[position_];
        if (c == '"') return parse_string(nullptr);
        if (c == '{') return parse_object(depth + 1);
        if (c == '[') return parse_array(depth + 1);
        if (c == 't') return parse_literal("true");
        if (c == 'f') return parse_literal("false");
        if (c == 'n') return parse_literal("null");
        return parse_number();
    }

    bool parse_object(unsigned int depth) {
        if (depth > kMaxDepth || !consume('{')) return false;
        skip_whitespace();
        if (consume('}')) return true;
        for (;;) {
            if (!parse_string(nullptr)) return false;
            skip_whitespace();
            if (!consume(':') || !parse_value(depth + 1)) return false;
            skip_whitespace();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            skip_whitespace();
        }
    }

    bool parse_array(unsigned int depth) {
        if (depth > kMaxDepth || !consume('[')) return false;
        skip_whitespace();
        if (consume(']')) return true;
        for (;;) {
            if (!parse_value(depth + 1)) return false;
            skip_whitespace();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            skip_whitespace();
        }
    }

    bool parse_literal(const char* value) {
        const size_t length = std::strlen(value);
        if (input_.compare(position_, length, value) != 0) return false;
        position_ += length;
        return true;
    }

    bool parse_number() {
        const size_t begin = position_;
        consume('-');
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                return false;
            }
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') return false;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            const size_t fraction_begin = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (position_ == fraction_begin) return false;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const size_t exponent_begin = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (position_ == exponent_begin) return false;
        }
        return position_ > begin;
    }

    const std::string& input_;
    size_t position_ = 0;
};

bool replace_dialog_type(const char* native_json, const char* custom_type, std::string* output) {
    if (native_json == nullptr || custom_type == nullptr || output == nullptr) return false;
    return JsonScanner(native_json).replace_top_level_type(custom_type, output);
}

QolResult choice_http_error(contracts::HttpRes* response, uint16_t status, const char* message);

QolResult query_response(contracts::HttpRes* response, const std::string& body) {
    if (response == nullptr || response->body == nullptr || response->body_capacity < body.size()) {
        return choice_http_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

QolResult state_query(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    const UiSnapshotCopy snapshot = copy_ui_snapshot();
    const ResolvedState top = resolve_top(snapshot);
    std::string body = "{\"name\":";
    body += json_quote(top.name.c_str());
    body += ",\"type\":";
    body += std::to_string(top.type);
    body += ",\"source\":";
    body += std::to_string(top.source);
    body += ",\"info\":";
    body += build_info_json(snapshot, top);
    body += ",\"options\":";
    body += build_options_json(snapshot, top);
    body += "}";
    return query_response(response, body);
}

QolResult table_query(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    std::vector<TableEntry> rows;
    collect_table(&rows);
    std::string body = "{\"states\":[";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) body += ",";
        body += "{\"name\":";
        body += json_quote(rows[i].name.c_str());
        body += ",\"type\":";
        body += std::to_string(rows[i].type);
        body += ",\"source\":";
        body += std::to_string(rows[i].source);
        body += ",\"stability\":\"";
        body += rows[i].stability == 0 ? "static" : "session";
        body += "\"}";
    }
    body += "]}";
    return query_response(response, body);
}

// 对话写（HTTP 写路由）的票据结果处理器 id（全局 typed handler id 空间；core.ui_state 前缀）。
constexpr uint32_t kChoiceWriteHandlerId = 0x4E430001;

struct ChoiceWritePayload {
    int32_t kind = 0;
    int32_t index = -1;
    char action[48] = {};
};

// Route contexts are mutable static objects because typed endpoint ctx is a void pointer.
int32_t kDialogSelectKind = contracts::kQolChoiceWriteDialogSelect;
int32_t kNpcInteractKind = contracts::kQolChoiceWriteNpcInteract;

const contracts::QolChoiceWritePort* g_choice_write = nullptr;

QolResult choice_http_error(contracts::HttpRes* response, uint16_t status, const char* message) {
    const std::string body = std::string("{\"ok\":false,\"error\":\"") + message + "\"}";
    if (response == nullptr || response->body == nullptr || response->body_capacity < body.size()) {
        return contracts::error(kQolErrInternal, "http response buffer too small", 31);
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = status;
    const int32_t code = status == 400 ? kQolErrInvalidArgument
                         : status == 503 ? kQolErrNotReady
                                          : kQolErrInternal;
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult choice_http_body(contracts::HttpRes* response, const std::string& body) {
    if (response == nullptr || response->body == nullptr ||
        response->body_capacity < body.size()) {
        return choice_http_error(response, 500, "http response buffer too small");
    }
    std::memcpy(response->body, body.data(), body.size());
    response->body_size = static_cast<uint32_t>(body.size());
    response->status = 200;
    return contracts::ok();
}

// body 字段提取（宽松）：body 语法拒绝留在 Kotlin 传输层（与 save/world 同口径）。
void choice_string_field(const char* body, const char* key, char* out, uint32_t capacity) {
    if (body == nullptr || key == nullptr || out == nullptr || capacity == 0) return;
    out[0] = '\0';
    const std::string needle = std::string("\"") + key + "\":\"";
    const std::string text(body);
    const size_t at = text.find(needle);
    if (at == std::string::npos) return;
    const size_t begin = at + needle.size();
    const size_t end = text.find('"', begin);
    if (end == std::string::npos) return;
    const std::string value = text.substr(begin, end - begin);
    std::snprintf(out, capacity, "%s", value.c_str());
}

void choice_int_field(const char* body, const char* key, int32_t* out) {
    if (body == nullptr || key == nullptr || out == nullptr) return;
    const std::string needle = std::string("\"") + key + "\":";
    const std::string text(body);
    const size_t at = text.find(needle);
    if (at == std::string::npos) return;
    *out = static_cast<int32_t>(std::strtol(text.c_str() + at + needle.size(), nullptr, 10));
}

// HTTP command handler（HTTP 线程）：只校验并投递票据，不直接写对话态。
QolResult choice_write_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                               void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return choice_http_error(response, 400, "bad request");
    }
    const int32_t kind = *static_cast<const int32_t*>(ctx);
    const std::string body_text(request->body != nullptr
                                    ? reinterpret_cast<const char*>(request->body)
                                    : "",
                                request->body_len);
    ChoiceWritePayload payload{};
    payload.kind = kind;
    if (kind == contracts::kQolChoiceWriteDialogSelect) {
        choice_string_field(body_text.c_str(), "action", payload.action,
                            static_cast<uint32_t>(sizeof(payload.action)));
        payload.index = -1;
        choice_int_field(body_text.c_str(), "index", &payload.index);
        if (payload.action[0] == '\0' && payload.index < 0) {
            return choice_http_error(response, 400, "action or index required");
        }
    }
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) {
        return choice_http_error(response, 503, "command port unavailable");
    }
    contracts::QolCommandTicket ticket = 0;
    if (!contracts::succeeded(g_sdk->commands.post_typed(kChoiceWriteHandlerId, &payload,
                                                        sizeof(payload), &ticket)) ||
        ticket == 0) {
        return choice_http_error(response, 503, "command queue unavailable");
    }
    return choice_http_body(response, std::string("{\"ok\":true,\"ticket\":") +
                                          std::to_string(ticket) + "}");
}

// 结果处理器（主线程）：执行对话写并写出迁移前的原响应 JSON。
bool choice_write_result(void*, const uint8_t* payload, uint32_t size, char* out, uint32_t capacity,
                         uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || size != sizeof(ChoiceWritePayload) || out == nullptr ||
        out_size == nullptr || g_choice_write == nullptr ||
        g_choice_write->read.write == nullptr) {
        return false;
    }
    ChoiceWritePayload p{};
    std::memcpy(&p, payload, size);
    const QolResult result = g_choice_write->read.write(p.kind, p.action, p.index, out, capacity,
                                                        out_size);
    return contracts::succeeded(result);
}

// 面板写（HTTP 写路由）使用独立的结果处理器 id：面板 kind 与对话 kind 各自从 0 起，
// 共用一个 payload/处理器会产生取值域歧义。
constexpr uint32_t kUiWriteHandlerId = 0x55490001;

struct UiWritePayload {
    uint64_t operation_id = 0;
    int32_t kind = 0;
    char panel[64] = {};
};

static_assert(sizeof(UiWritePayload) <= contracts::kTypedCommandPayloadCapacity,
              "UI write payload exceeds typed command capacity");

std::atomic<uint64_t> g_next_ui_operation_id{1};

uint64_t next_ui_operation_id() {
    uint64_t candidate = g_next_ui_operation_id.load(std::memory_order_relaxed);
    while (candidate != 0 && candidate != UINT64_MAX) {
        if (g_next_ui_operation_id.compare_exchange_weak(candidate, candidate + 1,
                                                         std::memory_order_relaxed,
                                                         std::memory_order_relaxed)) {
            return candidate;
        }
    }
    return 0;
}

int32_t kPanelOpenKind = contracts::kQolUiWritePanelOpen;
int32_t kPanelCloseKind = contracts::kQolUiWritePanelClose;
int32_t kMainMenuKind = contracts::kQolUiWriteMainMenu;

const contracts::QolUiWritePort* g_ui_write = nullptr;

// HTTP command handler（HTTP 线程）：只校验并投递票据，不直接改游戏面板栈。
QolResult ui_write_command(const contracts::HttpReq* request, contracts::HttpRes* response,
                           void* ctx) {
    if (request == nullptr || response == nullptr || ctx == nullptr) {
        return choice_http_error(response, 400, "bad request");
    }
    const int32_t kind = *static_cast<const int32_t*>(ctx);
    const std::string body_text(request->body != nullptr
                                    ? reinterpret_cast<const char*>(request->body)
                                    : "",
                                request->body_len);
    UiWritePayload payload{};
    payload.operation_id = next_ui_operation_id();
    if (payload.operation_id == 0) {
        return choice_http_error(response, 503, "command queue unavailable");
    }
    payload.kind = kind;
    if (kind == contracts::kQolUiWritePanelOpen) {
        choice_string_field(body_text.c_str(), "panel", payload.panel,
                            static_cast<uint32_t>(sizeof(payload.panel)));
        if (payload.panel[0] == '\0') {
            return choice_http_error(response, 400, "panel required");
        }
    }
    if (g_sdk == nullptr || g_sdk->commands.post_typed == nullptr) {
        return choice_http_error(response, 503, "command port unavailable");
    }
    contracts::QolCommandTicket ticket = 0;
    if (!contracts::succeeded(g_sdk->commands.post_typed(kUiWriteHandlerId, &payload,
                                                        sizeof(payload), &ticket)) ||
        ticket == 0) {
        return choice_http_error(response, 503, "command queue unavailable");
    }
    return choice_http_body(response, std::string("{\"ok\":true,\"ticket\":") +
                                          std::to_string(ticket) + "}");
}

// 结果处理器（主线程）：执行面板写并写出迁移前的原响应 JSON（附 state=<gamestate>）。
bool ui_write_result(void*, const uint8_t* payload, uint32_t size, char* out, uint32_t capacity,
                     uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || size != sizeof(UiWritePayload) || out == nullptr ||
        out_size == nullptr || g_ui_write == nullptr || g_ui_write->read.step == nullptr) {
        return false;
    }
    UiWritePayload p{};
    std::memcpy(&p, payload, size);
    const uint32_t attempt =
        (g_sdk != nullptr && g_sdk->ticket_result.current_attempt != nullptr)
            ? g_sdk->ticket_result.current_attempt()
            : 0;
    const QolResult result = g_ui_write->read.step(p.operation_id, attempt, p.kind, p.panel, out,
                                                  capacity, out_size);
    if (result.code == kQolErrNotReady) {
        const QolResult deferred =
            (g_sdk != nullptr && g_sdk->ticket_result.defer_current_result != nullptr)
                ? g_sdk->ticket_result.defer_current_result()
                : state_error(kQolErrNotReady, "UI write retry unavailable");
        if (contracts::succeeded(deferred)) return false;
        if (g_ui_write->read.cancel_pending != nullptr) {
            (void)g_ui_write->read.cancel_pending(p.operation_id);
        }
        constexpr char kPendingBody[] = "{\"ok\":false,\"error\":\"panel close pending\"}";
        if (sizeof(kPendingBody) - 1 <= capacity) {
            std::memcpy(out, kPendingBody, sizeof(kPendingBody) - 1);
            *out_size = static_cast<uint32_t>(sizeof(kPendingBody) - 1);
        }
        return false;
    }
    return contracts::succeeded(result);
}

const contracts::HttpEndpointDescriptor kHttpEndpoints[] = {
    {contracts::HttpMethod::kGet, "/api/ui/state", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1, &state_query, nullptr, nullptr},
    {contracts::HttpMethod::kGet, "/api/ui/state/table", kPluginId,
     contracts::HttpHandlerKind::kQuery, 0, 1, &table_query, nullptr, nullptr},
    {contracts::HttpMethod::kPost, "/api/ui/dialog/select", kPluginId,
     contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &choice_write_command,
     &kDialogSelectKind},
    {contracts::HttpMethod::kPost, "/api/ui/start_interact", kPluginId,
     contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &choice_write_command,
     &kNpcInteractKind},
    {contracts::HttpMethod::kPost, "/api/ui/open_panel", kPluginId,
     contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &ui_write_command,
     &kPanelOpenKind},
    {contracts::HttpMethod::kPost, "/api/ui/close_panel", kPluginId,
     contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &ui_write_command,
     &kPanelCloseKind},
    {contracts::HttpMethod::kPost, "/api/ui/go_main_menu", kPluginId,
     contracts::HttpHandlerKind::kCommand, 0, 1, nullptr, &ui_write_command,
     &kMainMenuKind},
};

int plugin_install(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_choice_write = nullptr;
    g_ui_write = nullptr;
    g_ui_state_read = nullptr;
    if (g_sdk == nullptr || ctx.ports == nullptr || ctx.port_count < 3 ||
        ctx.ports[0] == nullptr || ctx.ports[1] == nullptr || ctx.ports[2] == nullptr) {
        g_sdk = nullptr;
        return 1;
    }
    g_choice_write = static_cast<const contracts::QolChoiceWritePort*>(ctx.ports[0]);
    g_ui_write = static_cast<const contracts::QolUiWritePort*>(ctx.ports[1]);
    g_ui_state_read = static_cast<const contracts::QolUiStateReadPort*>(ctx.ports[2]);
    if (!contracts::satisfies(g_ui_write->abi_version,
                              {contracts::kQolPortUiWriteVersion.major,
                               contracts::kQolPortUiWriteVersion.minor}) ||
        g_choice_write->read.write == nullptr || g_ui_write->read.write == nullptr ||
        g_ui_write->read.step == nullptr ||
        g_ui_state_read->read.read_snapshot == nullptr ||
        g_sdk->scheduler.add == nullptr || g_sdk->scheduler.remove == nullptr) {
        g_sdk = nullptr;
        g_choice_write = nullptr;
        g_ui_write = nullptr;
        g_ui_state_read = nullptr;
        return 1;
    }
    // 结果处理器需要 host 尾部的票据结果面（QolHostSdk 尾部追加，按 abi 协商）。
    if (g_sdk->abi.size < sizeof(contracts::QolHostSdk) || g_sdk->abi.minor < 9 ||
        g_sdk->ticket_result.register_typed_result_handler == nullptr ||
        g_sdk->ticket_result.defer_current_result == nullptr ||
        g_sdk->ticket_result.current_attempt == nullptr ||
        !contracts::succeeded(g_sdk->ticket_result.register_typed_result_handler(
            kChoiceWriteHandlerId, &choice_write_result, nullptr))) {
        g_sdk = nullptr;
        g_choice_write = nullptr;
        g_ui_write = nullptr;
        g_ui_state_read = nullptr;
        return 1;
    }
    if (!contracts::succeeded(g_sdk->ticket_result.register_typed_result_handler(
            kUiWriteHandlerId, &ui_write_result, nullptr))) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kChoiceWriteHandlerId);
        g_sdk = nullptr;
        g_choice_write = nullptr;
        g_ui_write = nullptr;
        g_ui_state_read = nullptr;
        return 1;
    }
    if (!contracts::succeeded(g_sdk->scheduler.add(1, &native_snapshot_tick, nullptr,
                                                   1, 0, &g_snapshot_task))) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kChoiceWriteHandlerId);
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kUiWriteHandlerId);
        g_sdk = nullptr;
        g_choice_write = nullptr;
        g_ui_write = nullptr;
        g_ui_state_read = nullptr;
        g_snapshot_task = 0;
        return 1;
    }
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    if (g_ui_write != nullptr && g_ui_write->read.cancel_pending != nullptr) {
        (void)g_ui_write->read.cancel_pending(0);
    }
    if (g_sdk != nullptr && g_sdk->ticket_result.unregister_typed_result_handler != nullptr) {
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kChoiceWriteHandlerId);
        (void)g_sdk->ticket_result.unregister_typed_result_handler(kUiWriteHandlerId);
    }
    if (g_sdk != nullptr && g_sdk->scheduler.remove != nullptr && g_snapshot_task != 0) {
        (void)g_sdk->scheduler.remove(g_snapshot_task);
    }
    g_snapshot_task = 0;
    {
        std::lock_guard<std::mutex> lock(g_snapshot_mutex);
        g_snapshot = UiSnapshotState{};
    }
    g_sdk = nullptr;
    g_choice_write = nullptr;
    g_ui_write = nullptr;
    g_ui_state_read = nullptr;
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = true;  // 零门禁常驻
    out.error = nullptr;
}

// Feature callback does not replace the existing lifecycle or business controls.
constexpr contracts::FeatureDefinition kFeatures[] = {
    {"core.ui_state.provider", 1, contracts::FeatureCategory::kHidden,
     contracts::FeatureType::kToggle, "界面状态服务", "",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr, // no mandatory HTTP dependency
    2, kOptionalHttpRequires, // optional_requires=http.registry + http.dispatch
    1, kProvides, // provides=ui.state v1.0
    0, nullptr,   // resources（选择控件资源由 core.custom_ui 持有）
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（本插件已无资源）
    kPorts,
    3,
    nullptr,
    0,
    kFeatures, 1,
    kHttpEndpoints,
    static_cast<uint32_t>(sizeof(kHttpEndpoints) / sizeof(kHttpEndpoints[0])),
    nullptr,
    nullptr,
    0,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

}  // namespace qol::features::core_ui_state
