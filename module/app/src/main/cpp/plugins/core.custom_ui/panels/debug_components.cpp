#include "debug_components.h"

#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../contracts/http.h"
#include "../../../contracts/panels.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "../ui_components.h"
#include "stack.h"

namespace qol::features::core_ui {
namespace {

constexpr const char* kPanelId = "debug_components";
constexpr const char* kOwner = "core.custom_ui";
constexpr int64_t kFrameTopMargin = 18;
constexpr int64_t kFrameBottomMargin = 18;
constexpr int64_t kFrameRightMargin = 10;
constexpr int64_t kFrameLeftMargin = contracts::ui_theme::kNavBoxAbsX +
    contracts::ui_theme::kNavBoxW + 8;

contracts::UiRect g_panel_rect{0, 0, 960, 640};
contracts::UiRect g_outer_frame_rect{};
contracts::UiRect g_panel_nav_rect{};
const contracts::QolHostSdk* g_sdk = nullptr;
std::atomic<int32_t> g_requested_state{-1};
std::atomic<bool> g_enabled{false};
uint64_t g_apply_task = 0;
bool g_active = false;
contracts::QolUiHandle g_root = nullptr;
contracts::QolUiHandle g_panel_nav = nullptr;
contracts::QolUiHandle g_touch_root = nullptr;

void debug_log(const char* message) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr || message == nullptr) return;
    (void)g_sdk->log.write(1, 12, "core.custom_ui/debug_components", message,
                           static_cast<uint32_t>(std::strlen(message)));
}

void log_result(const char* action, QolResult result) {
    char message[96]{};
    std::snprintf(message, sizeof(message), "%s result=%d", action, result.code);
    debug_log(message);
}

int32_t color_channel(uint32_t color, uint32_t shift) {
    return static_cast<int32_t>((color >> shift) & 0xffu);
}

void draw_outer_panel_observation(const contracts::QolUiAdapter* adapter,
                                  const contracts::QolUiComponents* components) {
    if (adapter == nullptr || components == nullptr) return;
    if (components->draw_container_ornamented != nullptr) {
        (void)components->draw_container_ornamented(g_outer_frame_rect);
    }
    if (adapter->draw_string_with_font == nullptr || adapter->set_font_color_rgb == nullptr) return;

    const int32_t x = static_cast<int32_t>(g_outer_frame_rect.x + 28);
    const int32_t y = static_cast<int32_t>(g_outer_frame_rect.y + 30);
    (void)adapter->set_font_color_rgb(
        color_channel(contracts::ui_theme::kTextGold, 0),
        color_channel(contracts::ui_theme::kTextGold, 8),
        color_channel(contracts::ui_theme::kTextGold, 16));
    (void)adapter->draw_string_with_font("最大外层面板样例", x, y, 0, 0);

    char dimensions[96]{};
    std::snprintf(dimensions, sizeof(dimensions), "面板区域 %lld × %lld",
                  static_cast<long long>(g_panel_rect.w),
                  static_cast<long long>(g_panel_rect.h));
    (void)adapter->set_font_color_rgb(
        color_channel(contracts::ui_theme::kTextNormal, 0),
        color_channel(contracts::ui_theme::kTextNormal, 8),
        color_channel(contracts::ui_theme::kTextNormal, 16));
    (void)adapter->draw_string_with_font(dimensions, x, y + 48, 0, 0);

    if (adapter->fill_rect != nullptr) {
        (void)adapter->fill_rect({x, y + 82, 28, 20}, contracts::ui_theme::kPanelBg);
    }
    char background[64]{};
    const uint32_t panel_bg = contracts::ui_theme::kPanelBg;
    const uint32_t rgb = ((panel_bg & 0xffu) << 16) |
        (panel_bg & 0xff00u) | ((panel_bg >> 16) & 0xffu);
    std::snprintf(background, sizeof(background), "主题色参考 kPanelBg  #%06X", rgb);
    (void)adapter->draw_string_with_font(background, x + 40, y + 84, 0, 0);

    char frame_size[96]{};
    std::snprintf(frame_size, sizeof(frame_size), "容器外框 %lld × %lld",
                  static_cast<long long>(g_outer_frame_rect.w),
                  static_cast<long long>(g_outer_frame_rect.h));
    (void)adapter->draw_string_with_font(frame_size, x, y + 122, 0, 0);
}

void destroy_components() {
    const contracts::QolUiComponents* components = ui_components_api();
    if (g_root != nullptr && components != nullptr && components->destroy != nullptr) {
        (void)components->destroy(g_root);
    }
    g_root = nullptr;
    g_panel_nav = nullptr;
    g_touch_root = nullptr;
    g_active = false;
}

QolResult create_components() {
    const contracts::QolUiComponents* components = ui_components_api();
    const contracts::QolUiAdapter* adapter = ui_adapter();
    if (components == nullptr || adapter == nullptr || components->panel_create == nullptr ||
        components->nav_button_create == nullptr || components->nav_button_draw == nullptr ||
        components->destroy == nullptr) {
        return contracts::error(kQolErrNotReady, "debug ui components unavailable", 30);
    }
    if (adapter->screen_inset_width == nullptr || adapter->screen_inset_height == nullptr) {
        return contracts::error(kQolErrNotReady, "inset unavailable", 16);
    }
    int32_t inset_width = 0;
    int32_t inset_height = 0;
    if (!contracts::succeeded(adapter->screen_inset_width(&inset_width)) ||
        !contracts::succeeded(adapter->screen_inset_height(&inset_height)) ||
        inset_width < 0 || inset_height < 0) {
        return contracts::error(kQolErrInvalidArgument, "bad inset", 9);
    }
    const int64_t screen_width = 960 + static_cast<int64_t>(inset_width) * 2;
    const int64_t screen_height = 640 + static_cast<int64_t>(inset_height) * 2;
    if (screen_width < 960 || screen_height < 640) {
        return contracts::error(kQolErrInvalidArgument, "screen too small", 16);
    }
    g_panel_rect = {0, 0, screen_width, screen_height};
    g_outer_frame_rect = {kFrameLeftMargin, kFrameTopMargin,
        screen_width - kFrameLeftMargin - kFrameRightMargin,
        screen_height - kFrameTopMargin - kFrameBottomMargin};
    g_panel_nav_rect = {contracts::ui_theme::kNavBoxAbsX, contracts::ui_theme::kNavBoxAbsY,
                        contracts::ui_theme::kNavBoxW, contracts::ui_theme::kNavBoxH};

    const contracts::UiPanelDesc panel_desc{g_panel_rect, g_panel_rect, 0, 0};
    QolResult result = components->panel_create(&panel_desc, nullptr, &g_root);
    if (!contracts::succeeded(result) || g_root == nullptr) {
        destroy_components();
        return contracts::succeeded(result)
            ? contracts::error(kQolErrNotReady, "panel create returned null", 26) : result;
    }

    const contracts::UiNavButtonDesc nav_desc{
        g_panel_nav_rect, contracts::kUiNavButtonAuto, nullptr, nullptr};
    result = components->nav_button_create(&nav_desc, g_root, &g_panel_nav);
    if (!contracts::succeeded(result)) {
        destroy_components();
        return result;
    }
    if (adapter->image_unit_load != nullptr) (void)adapter->image_unit_load(0x0f);
    g_active = true;
    return contracts::ok();
}

void panel_enter() {
    debug_log("panel enter");
    if (g_root == nullptr) {
        const QolResult result = create_components();
        if (!contracts::succeeded(result)) log_result("create", result);
    } else {
        g_active = true;
    }
}

void panel_process() {
    if (!g_active || g_root == nullptr) return;
    const contracts::QolUiAdapter* adapter = ui_adapter();
    const contracts::QolUiComponents* components = ui_components_api();
    if (adapter == nullptr || components == nullptr || adapter->begin_frame == nullptr ||
        adapter->end_frame == nullptr) {
        return;
    }
    if (!contracts::succeeded(adapter->begin_frame())) return;
    draw_outer_panel_observation(adapter, components);
    if (g_panel_nav != nullptr && components->nav_button_draw != nullptr) {
        (void)components->nav_button_draw(g_panel_nav);
    }
    if (!contracts::succeeded(adapter->end_frame())) debug_log("end frame failed");
}

void panel_close() { destroy_components(); }
void panel_f3() { (void)ui_components_api()->back_request(); }
void panel_f4() { (void)ui_components_api()->back_request(); }

uint32_t panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (!g_active || g_root == nullptr) return 0;
    const contracts::QolUiComponents* components = ui_components_api();
    if (components == nullptr || components->dispatch_native_event == nullptr) return 1;
    if (event != 0x17 && event != 0x18 && event != 0x19) return 1;
    if (event == 0x17) g_touch_root = g_root;
    if (g_touch_root == nullptr) return 1;
    uint32_t consumed = 0;
    (void)components->dispatch_native_event(g_touch_root, event, param, param2, &consumed);
    if (event == 0x18) g_touch_root = nullptr;
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
    if (requested == 1) {
        const bool opened = ui_panels_open(kPanelId);
        char message[96]{};
        std::snprintf(message, sizeof(message), "apply requested=open result=%d active=%d",
                      opened ? 1 : 0, ui_panels_active(kPanelId) ? 1 : 0);
        debug_log(message);
    } else if (requested == 0) {
        ui_panels_close(kPanelId);
        char message[96]{};
        std::snprintf(message, sizeof(message), "apply requested=close active=%d",
                      ui_panels_active(kPanelId) ? 1 : 0);
        debug_log(message);
    }
    return 1;
}

}  // namespace

QolResult debug_components_http_command(const contracts::HttpReq* request,
                                        contracts::HttpRes* response, void* ctx) {
    return command_handler(request, response, ctx);
}

bool debug_components_register(const contracts::QolHostSdk* sdk) {
    if (sdk == nullptr || sdk->panels.register_provider == nullptr || sdk->scheduler.add == nullptr) {
        return false;
    }
    g_sdk = sdk;
    if (!contracts::succeeded(sdk->panels.register_provider(&kProvider))) return false;
    if (!contracts::succeeded(sdk->scheduler.add(1, &apply_request, nullptr, 1, 0,
                                                 &g_apply_task)) || g_apply_task == 0) {
        if (sdk->panels.unregister_owner != nullptr) (void)sdk->panels.unregister_owner(kOwner);
        g_sdk = nullptr;
        return false;
    }
    return true;
}

void debug_components_unregister(const contracts::QolHostSdk* sdk) {
    ui_panels_close(kPanelId);
    if (sdk != nullptr) {
        if (g_apply_task != 0 && sdk->scheduler.remove != nullptr) {
            (void)sdk->scheduler.remove(g_apply_task);
        }
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
