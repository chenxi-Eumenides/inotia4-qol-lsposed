#include "plugin.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

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
    uint64_t custom_dialog_last_generation = 0;
    uint64_t custom_dialog_sequence = 0;
};

std::mutex g_snapshot_mutex;
UiSnapshotState g_snapshot{};

const contracts::CapabilityRequirement kRequires[] = {
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
    }
    return contracts::ok();
}

const contracts::UiStateApi kUiStateApi{
    &publish_custom_panels,
    &publish_custom_dialog,
    &clear_custom_dialog,
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
    return copy;
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

QolResult screen_query(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    const UiSnapshotCopy snapshot = copy_ui_snapshot();
    if (!snapshot.native_snapshot_valid) {
        return choice_http_error(response, 503, "snapshot not ready");
    }
    return query_response(response, "{\"screen\":" + json_quote(snapshot.screen) + "}");
}

QolResult panel_query(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    const UiSnapshotCopy snapshot = copy_ui_snapshot();
    if (!snapshot.native_snapshot_valid) {
        return choice_http_error(response, 503, "snapshot not ready");
    }
    if (snapshot.custom_panel_count > 0) {
        const std::string panel = std::string("panel_") +
                                  snapshot.custom_panel_ids[snapshot.custom_panel_count - 1];
        return query_response(response, "{\"panel\":" + json_quote(panel.c_str()) + "}");
    }
    const std::string panel = snapshot.native_panel[0] == '\0'
                                  ? "null"
                                  : json_quote(snapshot.native_panel);
    return query_response(response, "{\"panel\":" + panel + "}");
}

QolResult dialog_query(const contracts::HttpReq*, contracts::HttpRes* response, void*) {
    const UiSnapshotCopy snapshot = copy_ui_snapshot();
    if (!snapshot.native_snapshot_valid) {
        return choice_http_error(response, 503, "snapshot not ready");
    }
    if (!snapshot.custom_dialog_active) return query_response(response, snapshot.native_dialog);
    std::string replaced;
    if (!replace_dialog_type(snapshot.native_dialog, snapshot.custom_dialog_type, &replaced)) {
        return choice_http_error(response, 503, "snapshot not ready");
    }
    return query_response(response, replaced);
}

// 对话写（HTTP 写路由）的票据结果处理器 id（全局 typed handler id 空间；core.ui_state 前缀）。
constexpr uint32_t kChoiceWriteHandlerId = 0x4E430001;

struct ChoiceWritePayload {
    int32_t kind = 0;
    int32_t index = -1;
    char action[48] = {};
};

// 路由 ctx：需为可取址的具名常量（HttpQueryHandlerDescriptor 存 void*）。
constexpr int32_t kDialogSelectKind = contracts::kQolChoiceWriteDialogSelect;
constexpr int32_t kNpcInteractKind = contracts::kQolChoiceWriteNpcInteract;

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
    int32_t kind = 0;
    char panel[64] = {};
};

constexpr int32_t kPanelOpenKind = contracts::kQolUiWritePanelOpen;
constexpr int32_t kPanelCloseKind = contracts::kQolUiWritePanelClose;
constexpr int32_t kMainMenuKind = contracts::kQolUiWriteMainMenu;

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
        out_size == nullptr || g_ui_write == nullptr || g_ui_write->read.write == nullptr) {
        return false;
    }
    UiWritePayload p{};
    std::memcpy(&p, payload, size);
    const QolResult result = g_ui_write->read.write(p.kind, p.panel, out, capacity, out_size);
    return contracts::succeeded(result);
}

// 查询与写入继续由同一 descriptor 提供，GET 路由已在 core.api/routes.inc 固定归属。
const contracts::HttpQueryHandlerDescriptor kRoutes[] = {
    {contracts::HttpMethod::kGet, "/api/ui/screen", kPluginId, &screen_query, nullptr},
    {contracts::HttpMethod::kGet, "/api/ui/panel", kPluginId, &panel_query, nullptr},
    {contracts::HttpMethod::kGet, "/api/ui/dialog", kPluginId, &dialog_query, nullptr},
    {contracts::HttpMethod::kPost, "/api/ui/dialog/select", kPluginId, &choice_write_command,
     const_cast<int32_t*>(&kDialogSelectKind)},
    {contracts::HttpMethod::kPost, "/api/ui/start_interact", kPluginId, &choice_write_command,
     const_cast<int32_t*>(&kNpcInteractKind)},
    {contracts::HttpMethod::kPost, "/api/ui/open_panel", kPluginId, &ui_write_command,
     const_cast<int32_t*>(&kPanelOpenKind)},
    {contracts::HttpMethod::kPost, "/api/ui/close_panel", kPluginId, &ui_write_command,
     const_cast<int32_t*>(&kPanelCloseKind)},
    {contracts::HttpMethod::kPost, "/api/ui/go_main_menu", kPluginId, &ui_write_command,
     const_cast<int32_t*>(&kMainMenuKind)},
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
    if (g_choice_write->read.write == nullptr || g_ui_write->read.write == nullptr ||
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
     contracts::FeatureType::kToggle, "界面状态服务", "提供游戏界面状态与原生界面操作命令。",
     [](contracts::PluginContext&, uint8_t) -> QolResult { return contracts::ok(); }, nullptr},
};

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    2, kRequires, // requires=http.registry + http.dispatch（声明式写路由）
    0, nullptr,   // optional_requires
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
    kRoutes,
    static_cast<uint32_t>(sizeof(kRoutes) / sizeof(kRoutes[0])),
    kFeatures, 1,
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

}  // namespace qol::features::core_ui_state
