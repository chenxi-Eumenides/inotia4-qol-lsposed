#include "debug_help.h"

#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../ui_components.h"
#include "stack.h"

namespace qol::features::core_ui {
namespace {
constexpr const char* kPanelId = "debug_help";
constexpr const char* kOwner = "core.custom_ui";
constexpr const char* kRoute = "/api/debug/custom-ui/help";
constexpr contracts::UiRect kBackRect{878, 554, 74, 81};
constexpr int32_t kRowHeight = 64;
constexpr const char* kTitleText = "两栏帮助页 / Help page";
constexpr const char* kHintText = "左栏上下拖动 · 轻触选择 · 右栏查看内容";
struct Sample {
    const char* title;
    const char* body;
    uint32_t color;
    uint32_t blocks;
};
Sample g_samples[] = {
    {"页面概览", "左侧选择主题，右侧显示对应内容。", 0xFFDAB878u, 1},
    {"原生滚动", "上下拖动左栏；滚动与惯性来自游戏。", 0xFF89BBADu, 2},
    {"选择高亮", "选中行带有边框与「选中」文字。", 0xFFC7A0D7u, 3},
    {"独立内容", "每个选项都有独立的内容回调。", 0xFF92B4D8u, 4},
    {"色块示例", "色块数量与颜色随选项变化。", 0xFFD79979u, 5},
    {"右栏边界", "右栏首版不滚动，由消费者安排内容。", 0xFFB9C983u, 6},
    {"末尾条目", "滑到底部后，仍可轻触切换内容。", 0xFFCF9EB3u, 7},
    {"返回与重开", "返回关闭页面；重新打开恢复首项。", 0xFFCDBD95u, 8},
};
constexpr uint32_t kCount = sizeof(g_samples) / sizeof(g_samples[0]);
const contracts::QolHostSdk* g_sdk = nullptr;
std::atomic<bool> g_enabled{false};
std::atomic<int32_t> g_request{-1};
uint64_t g_task = 0;
contracts::QolUiHandle g_root = nullptr, g_page = nullptr, g_back = nullptr;
contracts::QolUiHandle g_labels[3]{};
char g_row_text[kCount][128]{}, g_status[128]{};
bool g_active = false, g_registered = false;
int32_t g_last_selected = -1;
QolResult g_draw_result = contracts::ok();

const contracts::QolUiAdapter* adapter() { return g_sdk == nullptr ? nullptr : g_sdk->ui; }
bool main_thread() {
    const auto* ui = adapter();
    return ui != nullptr && ui->is_main_thread != nullptr && ui->is_main_thread() != 0;
}
void log_result(const char* action, QolResult result) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr) return;
    char message[128]{};
    std::snprintf(message, sizeof(message), "%s result=%d selected=%d", action, result.code,
                  g_last_selected);
    (void)g_sdk->log.write(1, 12, "core.custom_ui/debug_help", message,
                         static_cast<uint32_t>(std::strlen(message)));
}
void record(QolResult result) {
    if (contracts::succeeded(g_draw_result)) g_draw_result = result;
}
void draw_row(void*, uint32_t index, contracts::QolUiHandle row, uint8_t selected) {
    const auto* ui = adapter();
    const auto* components = ui_components_api();
    if (ui == nullptr || components == nullptr) return;
    char* text = g_row_text[index];
    std::snprintf(text, sizeof(g_row_text[index]), "%02u %s%s", index + 1,
                  g_samples[index].title, selected ? " [选中]" : "");
    // 背景/边框走绘制原语；文本经 draw_control_text，不写按钮 data 内联缓冲。
    record(ui->draw_custom_button(row, {0, 0, 244, kRowHeight},
        selected ? 0xFF51412Cu : 0xFF211D1Au, selected ? 0xFFE6C989u : 0xFF5B4C39u,
        selected ? 2 : 1, nullptr, 0, 0, 0));
    record(components->draw_control_text(row, text, 10, 20,
        selected ? 0xFFFFE9B4u : 0xFFF2E8D5u, 0, 1));
}
void draw_detail(void* user, contracts::UiRect viewport) {
    const auto& sample = *static_cast<Sample*>(user);
    const auto* ui = adapter();
    const auto x = static_cast<int32_t>(viewport.x), y = static_cast<int32_t>(viewport.y);
    char heading[128]{};
    std::snprintf(heading, sizeof(heading), "%02u / %02u  %s", sample.blocks, kCount, sample.title);
    record(ui->set_font_color(sample.color));
    record(ui->draw_string_with_font(heading, x + 20, y + 24, 0, 0));
    record(ui->set_font_color(0xFFF2E8D5u));
    record(ui->draw_string_with_font(sample.body, x + 20, y + 80, 0, 0));
    for (uint32_t i = 0; i < sample.blocks; ++i) {
        record(ui->fill_rect({x + 20 + i * 62, y + 146, 48, 64 + i * 8}, sample.color));
    }
    record(ui->set_font_color(0xFFC9BDABu));
    record(ui->draw_string_with_font("内容绘制在左栏裁剪结束之后。", x + 20, y + 290, 0, 0));
}

void panel_close() {
    if (!main_thread()) return;
    const auto* ui = adapter();
    if (g_page != nullptr) {
        const QolResult result = ui_components_api()->destroy(g_page);
        // unbind 已归还 owned page 时，延迟 close 只归还 borrowed popup root。
        if (!contracts::succeeded(result) && result.code != kQolErrNotFound) {
            log_result("destroy page", result); return;
        }
        g_page = nullptr;
    }
    if (g_root != nullptr) {
        const QolResult result = ui->delete_control(g_root);
        if (!contracts::succeeded(result)) { log_result("destroy root", result); return; }
        g_root = nullptr; g_back = nullptr;
        for (auto& label : g_labels) label = nullptr;
    }
    // 共享图集由游戏拥有、由返回按钮助手借用；关闭不释放物理贴图。
    g_active = false;
    log_result("close", contracts::ok());
    if (!g_registered) g_sdk = nullptr;
}

QolResult create_controls() {
    const auto* ui = adapter();
    if (!main_thread() || ui->create_control == nullptr || ui->set_rect == nullptr ||
        ui->control_absolute_origin == nullptr || ui->set_click_proc == nullptr ||
        ui->delete_control == nullptr ||
        ui->screen_inset_width == nullptr || ui->screen_inset_height == nullptr ||
        ui->begin_frame == nullptr || ui->end_frame == nullptr || ui->draw_custom_button == nullptr ||
        ui->set_font_color == nullptr || ui->set_font_color_rgb == nullptr ||
        ui->draw_string_with_font == nullptr || ui->fill_rect == nullptr ||
        ui->image_unit_load == nullptr) {
        return contracts::error(kQolErrNotReady, "help adapter unavailable", 24);
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
    contracts::UiHelpOption options[kCount]{};
    for (uint32_t i = 0; i < kCount; ++i) options[i] = {&g_samples[i], &draw_detail};
    const contracts::UiHelpPageDesc desc{{24, 104, 912, 408}, {20, 24, 244, 352},
        {320, 24, 568, 352}, kRowHeight, options, kCount, 0, &draw_row, nullptr};
    result = ui_components_api()->help_page_create(&desc, g_root, &g_page);
    if (!contracts::succeeded(result)) return result;
    const contracts::UiRect rects[] = {{24, 26, 850, 32}, {24, 64, 850, 32}, {32, 532, 780, 40}};
    // label 仅作定位锚点；文本在每帧用 draw_control_text 自绘，不写控件内联缓冲。
    for (uint32_t i = 0; i < 3; ++i) {
        result = ui->create_control(2, g_root, &g_labels[i]);
        if (!contracts::succeeded(result)) return result;
        result = ui->set_rect(g_labels[i], rects[i]);
        if (!contracts::succeeded(result)) return result;
    }
    // 返回按钮由助手创建（绑定宿主 back 语义）并负责 0x59 图集借用/绘制。
    result = ui_components_api()->back_button_create(g_root, kBackRect, &g_back);
    if (!contracts::succeeded(result)) return result;
    result = ui->image_unit_load(0x0f);
    if (!contracts::succeeded(result)) return result;
    g_last_selected = -1;
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
    if (g_page == nullptr || g_root == nullptr) return;
    const auto* ui = adapter();
    const auto* components = ui_components_api();
    QolResult result = components->help_page_process(g_page);
    if (!contracts::succeeded(result)) { log_result("process", result); (void)components->back_request(); return; }
    result = ui->begin_frame();
    if (!contracts::succeeded(result)) { log_result("begin frame", result); return; }
    g_draw_result = contracts::ok();
    record(ui->draw_custom_button(g_root, {0, 0, 960, 640}, 0xFF171512u, 0xFFB89B62u, 3,
                                 nullptr, 0, 0, 0));
    record(components->draw_control_text(g_labels[0], kTitleText, 0, 0, 0xFFFFE9B4u, 0, 1));
    record(components->draw_control_text(g_labels[1], kHintText, 0, 0, 0xFFC9BDABu, 0, 1));
    record(components->help_page_draw(g_page));
    int32_t selected = -1;
    record(components->help_page_get_selected(g_page, &selected));
    if (selected != g_last_selected) {
        g_last_selected = selected;
        log_result("select", contracts::ok());
    }
    std::snprintf(g_status, sizeof(g_status), "选中 %02d / %02u     左栏原生滚动 · 右栏独立绘制",
                  selected + 1, kCount);
    record(components->draw_control_text(g_labels[2], g_status, 0, 8, 0xFFE6C989u, 0, 1));
    record(components->back_button_draw(g_back));
    record(ui->end_frame());
    if (!contracts::succeeded(g_draw_result)) {
        log_result("draw", g_draw_result); (void)components->back_request();
    }
}
void panel_f3() { (void)ui_components_api()->back_request(); }
void panel_f4() { (void)ui_components_api()->back_request(); }
uint32_t panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (!main_thread() || !g_active || g_root == nullptr) return 1;
    uint32_t consumed = 0;
    const QolResult result = ui_components_api()->dispatch_native_event(g_root, event, param, param2,
                                                                        &consumed);
    if (!contracts::succeeded(result)) log_result("event", result);
    return consumed;
}
bool provider_open() {
    if (!main_thread() || !g_enabled.load(std::memory_order_acquire)) return false;
    g_active = true; return true;
}
bool provider_active() { return g_active; }
const char* block_reason() { return g_active ? "ui occupied: help page debug" : nullptr; }
const contracts::PanelCallbacks kCallbacks{&panel_enter, &panel_process, &panel_f3, &panel_f4, &panel_event};
const contracts::PanelProviderSpec kProvider{kPanelId, kOwner, &provider_open, &panel_close,
    &provider_active, &block_reason, &kCallbacks};

bool take(std::string_view& json, std::string_view token) {
    const size_t start = json.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return false;
    json.remove_prefix(start);
    if (json.substr(0, token.size()) != token) return false;
    json.remove_prefix(token.size()); return true;
}
QolResult respond(contracts::HttpRes* response, int32_t status, const char* text) {
    const auto size = static_cast<uint32_t>(std::strlen(text));
    if (response == nullptr || response->body == nullptr || response->body_capacity < size) {
        return contracts::error(kQolErrInvalidArgument, "response buffer unavailable", 27);
    }
    std::memcpy(response->body, text, size);
    response->body_size = size; response->status = status;
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
    g_request.store(open ? 1 : 0, std::memory_order_release);
    return respond(response, 200, "{\"ok\":true,\"queued\":true}");
}
int32_t apply_request(int64_t, void*) {
    if (!main_thread()) return 1;
    const int32_t request = g_request.exchange(-1, std::memory_order_acq_rel);
    if (!g_enabled.load(std::memory_order_acquire) || request == 0) ui_panels_close(kPanelId);
    else if (request == 1) {
        log_result("open request", ui_panels_open(kPanelId) ? contracts::ok() :
            contracts::error(kQolErrNotReady, "popup unavailable", 17));
    }
    return 1;
}
}  // namespace

bool debug_help_register(const contracts::QolHostSdk* sdk) {
    if (sdk == nullptr || sdk->panels.register_provider == nullptr ||
        sdk->http_dispatch.register_command == nullptr || sdk->scheduler.add == nullptr ||
        sdk->scheduler.remove == nullptr) return false;
    g_sdk = sdk;
    if (!contracts::succeeded(sdk->scheduler.add(1, &apply_request, nullptr, 1, 0, &g_task)) ||
        g_task == 0) { g_sdk = nullptr; return false; }
    if (!contracts::succeeded(sdk->panels.register_provider(&kProvider)) ||
        !contracts::succeeded(sdk->http_dispatch.register_command(contracts::HttpMethod::kPost,
            kRoute, kOwner, &command, nullptr))) {
        (void)sdk->scheduler.remove(g_task);
        g_task = 0; g_sdk = nullptr; return false;
    }
    g_registered = true; return true;
}
void debug_help_unregister(const contracts::QolHostSdk* sdk) {
    g_registered = false;
    g_enabled.store(false, std::memory_order_release);
    g_request.store(-1, std::memory_order_release);
    ui_panels_close(kPanelId);
    if (g_task != 0 && sdk != nullptr && sdk->scheduler.remove != nullptr) {
        (void)sdk->scheduler.remove(g_task);
    }
    g_task = 0;
    // 共享 owner 的面板/路由由插件统一撤销；延迟 close 完成前保留 SDK。
    if (!g_active) g_sdk = nullptr;
}
void debug_help_set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) g_request.store(0, std::memory_order_release);
}
}  // namespace qol::features::core_ui
#endif
