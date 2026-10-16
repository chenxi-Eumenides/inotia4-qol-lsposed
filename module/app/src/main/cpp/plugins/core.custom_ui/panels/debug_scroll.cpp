#include "debug_scroll.h"

#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../ui_components.h"
#include "stack.h"

namespace qol::features::core_ui {
namespace {

constexpr const char* kPanelId = "debug_scroll";
constexpr const char* kOwner = "core.custom_ui";
constexpr contracts::UiRect kViewport{48, 110, 760, 392};
constexpr contracts::UiRect kBackRect{878, 554, 74, 81};
constexpr int32_t kRowHeight = 56;
constexpr uint32_t kRowCount = 24;
constexpr const char* kTitleText = "原生滚动列表 / ControlScroll";
constexpr const char* kHintText = "上下拖动浏览 24 行 · 轻触选择 · 返回关闭";
const contracts::QolHostSdk* g_sdk = nullptr;
std::atomic<bool> g_enabled{false};
std::atomic<int32_t> g_request{-1};
uint64_t g_task = 0;
contracts::QolUiHandle g_root = nullptr;
contracts::QolUiHandle g_list = nullptr;
contracts::QolUiHandle g_back = nullptr;
contracts::QolUiHandle g_labels[3]{};
char g_row_text[kRowCount][128]{};
char g_status_text[128]{};
bool g_active = false;
bool g_registered = false;
int32_t g_selected = 0;

const contracts::QolUiAdapter* adapter() { return g_sdk == nullptr ? nullptr : g_sdk->ui; }

bool main_thread() {
    const auto* ui = adapter();
    return ui != nullptr && ui->is_main_thread != nullptr && ui->is_main_thread() != 0;
}

void log_result(const char* action, QolResult result) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr) return;
    char message[128]{};
    std::snprintf(message, sizeof(message), "%s result=%d selected=%d", action, result.code, g_selected);
    (void)g_sdk->log.write(1, 12, "core.custom_ui/debug_scroll", message,
                         static_cast<uint32_t>(std::strlen(message)));
}

void selected(void*, uint32_t index) {
    g_selected = static_cast<int32_t>(index);
    log_result("select", contracts::ok());
}

void draw_row(void*, uint32_t index, contracts::QolUiHandle row, uint8_t is_selected) {
    const auto* ui = adapter();
    const auto* components = ui_components_api();
    if (ui == nullptr || components == nullptr) return;
    char* text = g_row_text[index];
    std::snprintf(text, sizeof(g_row_text[index]), "%02u  原生滚动列表 · 测试条目 %02u%s",
                  index + 1, index + 1, is_selected ? "  [选中]" : "");
    // 背景/边框走绘制原语；text=nullptr 使 draw_custom_button 不读控件内联文本。
    const QolResult bg = ui->draw_custom_button(row, {0, 0, kViewport.w, kRowHeight},
        is_selected ? 0xFF51412Cu : (index % 2 == 0 ? 0xFF211D1Au : 0xFF29231Eu),
        is_selected ? 0xFFE6C989u : 0xFF5B4C39u, is_selected ? 2 : 1, nullptr, 0, 0, 0);
    if (!contracts::succeeded(bg)) { log_result("draw row bg", bg); return; }
    // 文本经独立绘制原语，不写按钮 data 内联缓冲（0x20 字节，长串会覆盖 ExecuteProc）。
    const QolResult fg = components->draw_control_text(row, text, 16, 16,
        is_selected ? 0xFFFFE9B4u : 0xFFF2E8D5u, 0, 1);
    if (!contracts::succeeded(fg)) log_result("draw row text", fg);
}

void panel_close() {
    if (!main_thread()) return;
    const auto* ui = adapter();
    if (g_list != nullptr) {
        const QolResult result = ui_components_api()->destroy(g_list);
        // 插件 unbind 已归还 owned 列表时，延迟的宿主 close 只需归还 borrowed root。
        if (!contracts::succeeded(result) && result.code != kQolErrNotFound) {
            log_result("destroy list", result); return;
        }
        g_list = nullptr;
    }
    if (g_root != nullptr) {
        const QolResult result = ui->delete_control(g_root);
        if (!contracts::succeeded(result)) { log_result("destroy root", result); return; }
        g_root = nullptr;
        g_back = nullptr;
        for (auto& label : g_labels) label = nullptr;
    }
    // option 图集由游戏共享、由返回按钮助手借用；页面关闭不释放物理贴图。
    g_active = false;
    log_result("close", contracts::ok());
    if (!g_registered) g_sdk = nullptr;
}

QolResult create_controls() {
    const auto* ui = adapter();
    if (!main_thread() || ui->create_control == nullptr || ui->set_rect == nullptr ||
        ui->get_rect == nullptr || ui->set_click_proc == nullptr || ui->delete_control == nullptr ||
        ui->control_absolute_origin == nullptr || ui->set_font_color_rgb == nullptr ||
        ui->draw_string_with_font == nullptr ||
        ui->screen_inset_width == nullptr || ui->screen_inset_height == nullptr ||
        ui->begin_frame == nullptr || ui->end_frame == nullptr || ui->draw_custom_button == nullptr) {
        return contracts::error(kQolErrNotReady, "scroll popup adapter unavailable", 32);
    }
    int32_t inset_x = 0, inset_y = 0;
    QolResult result = ui->screen_inset_width(&inset_x);
    if (!contracts::succeeded(result)) return result;
    result = ui->screen_inset_height(&inset_y);
    if (!contracts::succeeded(result)) return result;
    if (inset_x < 0 || inset_y < 0) return contracts::error(kQolErrInvalidArgument, "bad inset", 9);
    result = ui->create_control(0, nullptr, &g_root);
    if (!contracts::succeeded(result)) return result;
    result = ui->set_rect(g_root, {inset_x, inset_y, 960, 640});
    if (!contracts::succeeded(result)) return result;
    const contracts::UiScrollListDesc desc{
        kViewport, kRowHeight, kRowCount, 0, &draw_row, &selected, nullptr};
    result = ui_components_api()->scroll_list_create(&desc, g_root, &g_list);
    if (!contracts::succeeded(result)) return result;
    const contracts::UiRect label_rects[] = {
        {48, 26, 820, 32}, {48, 64, 820, 32}, {48, 526, 780, 40}};
    // label 仅作定位锚点；文本在每帧用 draw_control_text 自绘，不写控件内联缓冲（超 31B 会越界）。
    for (uint32_t i = 0; i < 3; ++i) {
        result = ui->create_control(2, g_root, &g_labels[i]);  // label 不激活触摸。
        if (!contracts::succeeded(result)) return result;
        result = ui->set_rect(g_labels[i], label_rects[i]);
        if (!contracts::succeeded(result)) return result;
    }
    // 返回按钮由助手创建（绑定宿主 back 语义）并负责 0x59 图集借用/绘制。
    result = ui_components_api()->back_button_create(g_root, kBackRect, &g_back);
    if (!contracts::succeeded(result)) return result;
    g_selected = 0;
    return contracts::ok();
}

void panel_enter() {
    if (!main_thread() || g_root != nullptr) return;
    const QolResult result = create_controls();
    log_result("enter", result);
    if (!contracts::succeeded(result)) (void)ui_components_api()->back_request();
}

void panel_process() {
    if (!main_thread() || !g_active) return;
    if (g_list == nullptr || g_root == nullptr) return;
    const auto* ui = adapter();
    const auto* components = ui_components_api();
    QolResult result = components->scroll_list_process(g_list);
    if (!contracts::succeeded(result)) { log_result("process", result); return; }
    result = ui->begin_frame();
    if (!contracts::succeeded(result)) { log_result("begin frame", result); return; }
    (void)ui->draw_custom_button(g_root, {0, 0, 960, 640}, 0xFF171512u, 0xFFB89B62u, 3,
        nullptr, 0, 0, 0);
    (void)components->draw_control_text(g_labels[0], kTitleText, 0, 0, 0xFFFFE9B4u, 0, 1);
    (void)components->draw_control_text(g_labels[1], kHintText, 0, 0, 0xFFC9BDABu, 0, 1);
    contracts::UiRect root_rect{};
    if (contracts::succeeded(ui->get_rect(g_root, &root_rect))) {
        result = components->draw_container({root_rect.x + kViewport.x - 8,
            root_rect.y + kViewport.y - 8, kViewport.w + 32, kViewport.h + 16});
        if (!contracts::succeeded(result)) log_result("container", result);
    }
    result = components->scroll_list_draw(g_list);
    if (!contracts::succeeded(result)) log_result("draw list", result);
    int64_t offset = 0;
    result = components->scroll_list_get_offset(g_list, &offset);
    if (contracts::succeeded(result)) {
        std::snprintf(g_status_text, sizeof(g_status_text), "选中 %02d / 24     原生 offset: %lld / %lld",
            g_selected + 1, static_cast<long long>(offset),
            static_cast<long long>(kRowCount * kRowHeight - kViewport.h));
    } else {
        std::snprintf(g_status_text, sizeof(g_status_text), "原生 offset 读取失败 (%d)", result.code);
    }
    result = components->draw_control_text(g_labels[2], g_status_text, 0, 8, 0xFFE6C989u, 0, 1);
    if (!contracts::succeeded(result)) log_result("draw status text", result);
    result = components->back_button_draw(g_back);
    if (!contracts::succeeded(result)) log_result("draw back", result);
    result = ui->end_frame();
    if (!contracts::succeeded(result)) log_result("end frame", result);
}

void panel_f3() { (void)ui_components_api()->back_request(); }
void panel_f4() { (void)ui_components_api()->back_request(); }
uint32_t panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (!main_thread() || !g_active || g_root == nullptr) return 1;
    uint32_t consumed = 0;
    const QolResult result = ui_components_api()->dispatch_native_event(
        g_root, event, param, param2, &consumed);
    if (!contracts::succeeded(result)) log_result("event", result);
    return consumed;
}

bool provider_open() {
    if (!main_thread() || !g_enabled.load(std::memory_order_acquire)) return false;
    g_active = true;
    return true;
}
bool provider_active() { return g_active; }
const char* provider_block_reason() { return g_active ? "ui occupied: native scroll debug" : nullptr; }
const contracts::PanelCallbacks kCallbacks{
    &panel_enter, &panel_process, &panel_f3, &panel_f4, &panel_event};
const contracts::PanelProviderSpec kProvider{
    kPanelId, kOwner, &provider_open, &panel_close, &provider_active, &provider_block_reason, &kCallbacks};

bool take(std::string_view& json, std::string_view token) {
    const size_t start = json.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return false;
    json.remove_prefix(start);
    if (json.substr(0, token.size()) != token) return false;
    json.remove_prefix(token.size());
    return true;
}

QolResult respond(contracts::HttpRes* response, int32_t status, const char* text) {
    const auto size = static_cast<uint32_t>(std::strlen(text));
    if (response == nullptr || response->body == nullptr || response->body_capacity < size) {
        return contracts::error(kQolErrInvalidArgument, "response buffer unavailable", 27);
    }
    std::memcpy(response->body, text, size);
    response->body_size = size;
    response->status = status;
    return contracts::ok();
}

QolResult command(const contracts::HttpReq* request, contracts::HttpRes* response, void*) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return respond(response, 503, "{\"ok\":false,\"error\":\"custom ui disabled\"}");
    }
    if (request == nullptr || request->body == nullptr) {
        return respond(response, 400, "{\"ok\":false,\"error\":\"open boolean required\"}");
    }
    std::string_view json(reinterpret_cast<const char*>(request->body), request->body_len);
    bool open = false;
    if (!take(json, "{") || !take(json, "\"open\"") || !take(json, ":") ||
        (!(open = take(json, "true")) && !take(json, "false")) || !take(json, "}") ||
        json.find_first_not_of(" \t\r\n") != std::string_view::npos) {
        return respond(response, 400, "{\"ok\":false,\"error\":\"open boolean required\"}");
    }
    // HTTP 线程只写原子请求，最后一个请求优先；控件/宿主操作均在帧任务执行。
    g_request.store(open ? 1 : 0, std::memory_order_release);
    return respond(response, 200, "{\"ok\":true,\"queued\":true}");
}

int32_t apply_request(int64_t, void*) {
    if (!main_thread()) return 1;
    const int32_t request = g_request.exchange(-1, std::memory_order_acq_rel);
    if (!g_enabled.load(std::memory_order_acquire) || request == 0) ui_panels_close(kPanelId);
    else if (request == 1) {
        const bool opened = ui_panels_open(kPanelId);
        log_result("open request", opened ? contracts::ok() :
            contracts::error(kQolErrNotReady, "popup unavailable", 17));
    }
    return 1;
}

}  // namespace

QolResult debug_scroll_http_command(const contracts::HttpReq* request,
                                    contracts::HttpRes* response, void* ctx) {
    return command(request, response, ctx);
}

bool debug_scroll_register(const contracts::QolHostSdk* sdk) {
    if (sdk == nullptr || sdk->panels.register_provider == nullptr ||
        sdk->scheduler.add == nullptr || sdk->scheduler.remove == nullptr) return false;
    g_sdk = sdk;
    if (!contracts::succeeded(sdk->scheduler.add(1, &apply_request, nullptr, 1, 0, &g_task)) ||
        g_task == 0) { g_sdk = nullptr; return false; }
    if (!contracts::succeeded(sdk->panels.register_provider(&kProvider))) {
        (void)sdk->scheduler.remove(g_task);
        g_task = 0;
        g_sdk = nullptr;
        return false;
    }
    g_registered = true;
    return true;
}

void debug_scroll_unregister(const contracts::QolHostSdk* sdk) {
    g_registered = false;
    g_enabled.store(false, std::memory_order_release);
    g_request.store(-1, std::memory_order_release);
    ui_panels_close(kPanelId);
    if (g_task != 0 && sdk != nullptr && sdk->scheduler.remove != nullptr) {
        (void)sdk->scheduler.remove(g_task);
    }
    g_task = 0;
    // 两个 debug provider 共享 owner，插件统一通过 debug_components_unregister 撤销登记。
    // 宿主 snapshot 门可能延迟 close；保留静态 SDK，直到 provider.close 归还控件。
    if (!g_active) g_sdk = nullptr;
}

void debug_scroll_set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) g_request.store(0, std::memory_order_release);
}

}  // namespace qol::features::core_ui
#endif
