#include "debug_components.h"

#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "../../../contracts/http.h"
#include "../../../contracts/panels.h"
#include "../../../contracts/ui.h"
#include "../ui_components.h"
#include "stack.h"

namespace qol::features::core_ui {
namespace {

constexpr const char* kPanelId = "debug_components";
constexpr const char* kOwner = "core.custom_ui";
constexpr const char* kRoute = "/api/debug/custom-ui/components";
constexpr contracts::UiRect kPanelRect{0, 0, 720, 448};
constexpr int64_t kRowTop = 64;
constexpr int64_t kRowHeight = 68;
constexpr int64_t kLabelX = 28;
constexpr int64_t kLabelWidth = 228;
constexpr int64_t kControlX = 276;
constexpr int64_t kControlWidth = 408;
constexpr int64_t kControlHeight = 48;

const contracts::QolHostSdk* g_sdk = nullptr;
std::atomic<int32_t> g_requested_state{-1};
std::atomic<bool> g_enabled{false};
uint64_t g_apply_task = 0;
bool g_active = false;
contracts::QolUiHandle g_root = nullptr;
contracts::QolUiHandle g_labels[5]{};
contracts::QolUiHandle g_samples[4]{};

void destroy_components() {
    if (g_root != nullptr) {
        const contracts::QolUiComponents* components = ui_components_api();
        if (components != nullptr && components->destroy != nullptr) {
            (void)components->destroy(g_root);
        }
    }
    g_root = nullptr;
    for (auto& label : g_labels) label = nullptr;
    for (auto& sample : g_samples) sample = nullptr;
    g_active = false;
}

bool create_components() {
    const contracts::QolUiComponents* components = ui_components_api();
    const contracts::QolUiAdapter* adapter = ui_adapter();
    if (components == nullptr || adapter == nullptr || components->panel_create == nullptr ||
        components->label_create == nullptr || components->button_create == nullptr ||
        components->toggle_create == nullptr || components->select_create == nullptr ||
        components->scroll_view_create == nullptr) {
        return false;
    }

    int32_t inset_width = 0;
    int32_t inset_height = 0;
    if (adapter->screen_inset_width == nullptr || adapter->screen_inset_height == nullptr ||
        !contracts::succeeded(adapter->screen_inset_width(&inset_width)) ||
        !contracts::succeeded(adapter->screen_inset_height(&inset_height))) {
        return false;
    }
    const int64_t screen_width = 960 + static_cast<int64_t>(inset_width) * 2;
    const int64_t screen_height = 640 + static_cast<int64_t>(inset_height) * 2;
    if (screen_width < kPanelRect.w || screen_height < kPanelRect.h) return false;
    const contracts::UiRect rect{(screen_width - kPanelRect.w) / 2,
                                 (screen_height - kPanelRect.h) / 2,
                                 kPanelRect.w, kPanelRect.h};

    const contracts::UiPanelDesc panel_desc{rect, rect, 0, 0};
    if (!contracts::succeeded(components->panel_create(&panel_desc, nullptr, &g_root))) {
        destroy_components();
        return false;
    }
    const char* const labels[] = {"label", "button", "toggle", "select", "scroll_view"};
    for (int32_t i = 0; i < 5; ++i) {
        const contracts::UiLabelDesc desc{
            {kLabelX, kRowTop + i * kRowHeight, kLabelWidth, kControlHeight},
            labels[i], 0xFFFFFFFFu, contracts::UiAlign::kLeft};
        if (!contracts::succeeded(components->label_create(&desc, g_root, &g_labels[i]))) {
            destroy_components();
            return false;
        }
    }

    const contracts::UiButtonDesc button_desc{
        {kControlX, kRowTop + kRowHeight, kControlWidth, kControlHeight},
        "button", 0, nullptr, nullptr};
    const contracts::UiToggleDesc toggle_desc{
        {kControlX, kRowTop + 2 * kRowHeight, kControlWidth, kControlHeight},
        "toggle", 0, 0, nullptr, nullptr};
    const char* options[] = {"first", "second"};
    const contracts::UiSelectDesc select_desc{
        {kControlX, kRowTop + 3 * kRowHeight, kControlWidth, kControlHeight},
        options, 2, 0, 0, nullptr, nullptr};
    const contracts::UiScrollViewDesc scroll_desc{
        {kControlX, kRowTop + 4 * kRowHeight, kControlWidth, kControlHeight},
        144, 0, static_cast<int32_t>(kControlHeight), 0, nullptr, nullptr};
    if (!contracts::succeeded(components->button_create(&button_desc, g_root, &g_samples[0])) ||
        !contracts::succeeded(components->toggle_create(&toggle_desc, g_root, &g_samples[1])) ||
        !contracts::succeeded(components->select_create(&select_desc, g_root, &g_samples[2])) ||
        !contracts::succeeded(components->scroll_view_create(&scroll_desc, g_root, &g_samples[3]))) {
        destroy_components();
        return false;
    }
    g_active = true;
    return true;
}

void panel_enter() {
    if (g_root == nullptr) (void)create_components();
    else g_active = true;
}

void panel_process() {
    if (!g_active || g_root == nullptr) return;
    const contracts::QolUiAdapter* adapter = ui_adapter();
    if (adapter == nullptr || adapter->begin_frame == nullptr || adapter->end_frame == nullptr ||
        adapter->fill_rect_alpha == nullptr || adapter->draw_control == nullptr) {
        return;
    }
    if (!contracts::succeeded(adapter->begin_frame())) return;
    contracts::UiRect rect{};
    if (adapter->get_rect != nullptr && contracts::succeeded(adapter->get_rect(g_root, &rect))) {
        (void)adapter->fill_rect_alpha(rect, 0xFF101820u, 0xF0u);
        for (int32_t row = 0; row < 5; ++row) {
            (void)adapter->draw_control(g_labels[row]);
            if (row > 0) (void)adapter->draw_control(g_samples[row - 1]);
        }
    }
    (void)adapter->end_frame();
}

void panel_close() { destroy_components(); }

void panel_f3() { ui_panels_close(kPanelId); }

void panel_f4() { ui_panels_close(kPanelId); }

uint32_t panel_event(uint64_t event, uint64_t param, uint64_t) {
    if (!g_active || g_root == nullptr || param == 0) return 1;
    uint32_t phase = 0;
    if (event == 0x17) phase = 1;
    else if (event == 0x18) phase = 2;
    else return 1;
    const contracts::QolUiAdapter* adapter = ui_adapter();
    const contracts::QolUiComponents* components = ui_components_api();
    if (adapter == nullptr || adapter->touch_coordinates == nullptr || components == nullptr ||
        components->dispatch_touch == nullptr) {
        return 1;
    }
    int64_t x = 0;
    int64_t y = 0;
    if (contracts::succeeded(adapter->touch_coordinates(param, &x, &y))) {
        (void)components->dispatch_touch(g_root, phase, x, y);
    }
    return 1;
}

const contracts::PanelCallbacks kCallbacks{
    &panel_enter, &panel_process, &panel_f3, &panel_f4, &panel_event};

bool provider_open() { return g_sdk != nullptr; }
void provider_close() { panel_close(); }
bool provider_active() { return g_active; }
const char* provider_block_reason() { return g_active ? "ui occupied: debug components" : nullptr; }

const contracts::PanelProviderSpec kProvider{
    kPanelId, kOwner, &provider_open, &provider_close, &provider_active,
    &provider_block_reason, &kCallbacks};

QolResult write_response(contracts::HttpRes* response, const char* body) {
    if (response == nullptr || response->body == nullptr || body == nullptr) {
        return contracts::error(kQolErrInvalidArgument, "bad request", 11);
    }
    const uint32_t size = static_cast<uint32_t>(std::strlen(body));
    if (response->body_capacity < size) {
        response->status = 500;
        return contracts::error(kQolErrInternal, "response buffer too small", 25);
    }
    std::memcpy(response->body, body, size);
    response->body_size = size;
    response->status = 200;
    return contracts::ok();
}

QolResult command_handler(const contracts::HttpReq* request, contracts::HttpRes* response, void*) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return write_response(response, "{\"ok\":false,\"error\":\"custom ui disabled\"}");
    }
    if (request == nullptr || request->body == nullptr || request->body_len == 0) {
        return write_response(response, "{\"ok\":false,\"error\":\"bad request\"}");
    }
    const char* body = reinterpret_cast<const char*>(request->body);
    const std::string_view json(body, request->body_len);
    const size_t key = json.find("\"open\"");
    if (key == std::string_view::npos) {
        return write_response(response, "{\"ok\":false,\"error\":\"open required\"}");
    }
    const size_t colon = json.find(':', key + 6);
    if (colon == std::string_view::npos) {
        return write_response(response, "{\"ok\":false,\"error\":\"bad request\"}");
    }
    const size_t value = json.find_first_not_of(" \t\r\n", colon + 1);
    if (value == std::string_view::npos ||
        (json.compare(value, 4, "true") != 0 && json.compare(value, 5, "false") != 0)) {
        return write_response(response, "{\"ok\":false,\"error\":\"open must be boolean\"}");
    }
    const size_t value_size = json.compare(value, 4, "true") == 0 ? 4 : 5;
    const size_t end = json.find_first_not_of(" \t\r\n", value + value_size);
    if (end != std::string_view::npos && json[end] != ',' && json[end] != '}') {
        return write_response(response, "{\"ok\":false,\"error\":\"open must be boolean\"}");
    }
    g_requested_state.store(value_size == 4 ? 1 : 0, std::memory_order_release);
    return write_response(response, "{\"ok\":true,\"queued\":true}");
}

int32_t apply_request(int64_t, void*) {
    const int32_t requested = g_requested_state.exchange(-1, std::memory_order_acq_rel);
    if (requested == 1) (void)ui_panels_open(kPanelId);
    else if (requested == 0) ui_panels_close(kPanelId);
    return 1;
}

}  // namespace

bool debug_components_register(const contracts::QolHostSdk* sdk) {
    if (sdk == nullptr || sdk->panels.register_provider == nullptr ||
        sdk->http_dispatch.register_command == nullptr || sdk->scheduler.add == nullptr) {
        return false;
    }
    g_sdk = sdk;
    if (!contracts::succeeded(sdk->panels.register_provider(&kProvider))) return false;
    if (!contracts::succeeded(sdk->http_dispatch.register_command(
            contracts::HttpMethod::kPost, kRoute, kOwner, &command_handler, nullptr))) {
        if (sdk->panels.unregister_owner != nullptr) {
            (void)sdk->panels.unregister_owner(kOwner);
        }
        g_sdk = nullptr;
        return false;
    }
    if (!contracts::succeeded(sdk->scheduler.add(1, &apply_request, nullptr, 1, 0,
                                                 &g_apply_task)) || g_apply_task == 0) {
        (void)sdk->http_dispatch.unregister_owner(kOwner);
        if (sdk->panels.unregister_owner != nullptr) (void)sdk->panels.unregister_owner(kOwner);
        g_sdk = nullptr;
        return false;
    }
    return true;
}

void debug_components_unregister(const contracts::QolHostSdk* sdk) {
    ui_panels_close(kPanelId);
    if (sdk != nullptr) {
        if (g_apply_task != 0 && sdk->scheduler.remove != nullptr)
            (void)sdk->scheduler.remove(g_apply_task);
        if (sdk->http_dispatch.unregister_owner != nullptr)
            (void)sdk->http_dispatch.unregister_owner(kOwner);
        if (sdk->panels.unregister_owner != nullptr) (void)sdk->panels.unregister_owner(kOwner);
    }
    g_apply_task = 0;
    g_requested_state.store(-1, std::memory_order_release);
    g_enabled.store(false, std::memory_order_release);
    g_sdk = nullptr;
}

void debug_components_set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        g_requested_state.store(-1, std::memory_order_release);
        ui_panels_close(kPanelId);
    }
}

}  // namespace qol::features::core_ui

#else

namespace qol::features::core_ui {
bool debug_components_register(const contracts::QolHostSdk*) { return true; }
void debug_components_unregister(const contracts::QolHostSdk*) {}
void debug_components_set_enabled(bool) {}
}  // namespace qol::features::core_ui

#endif
