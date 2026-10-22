#include "panel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "../../../contracts/panels.h"
#include "../../../contracts/static_data.h"
#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "../feature.h"
#include "../map_data.h"

namespace qol::features::custom_map {
namespace {
using contracts::QolUiHandle;
using contracts::UiRect;

constexpr int64_t kPanelW = 960;
constexpr int64_t kPanelH = 640;
constexpr UiRect kLeftView{24, 100, 280, 444};
constexpr UiRect kMapView{344, 100, 592, 444};
constexpr int32_t kRowHeight = 54;
constexpr const char* kDefaultPackage =
    "com.com2us.inotia4.normal.freefull.google.global.android.common";

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolUiComponents* g_components = nullptr;
const contracts::QolUiAdapter* g_ui = nullptr;
const contracts::QolStaticDataApi* g_static_data = nullptr;
bool g_active = false;
uint64_t g_frame = 0;
int32_t g_pending_selection = -1;
int32_t g_selected = -1;
QolUiHandle g_root = nullptr;
QolUiHandle g_left = nullptr;
QolUiHandle g_back = nullptr;
QolUiHandle g_pressed_list = nullptr;
bool g_back_pressed = false;
std::vector<CustomMapSummary> g_maps;
CustomMapDetail g_detail{};
std::vector<uint32_t> g_tile_colors;
std::vector<uint8_t> g_exit_mask;
std::string g_map_name;
std::string g_map_directory;
std::string g_status;

constexpr std::array<uint32_t, 12> kTilePalette{
    0xFF535A4Bu, 0xFF586B57u, 0xFF4D6875u, 0xFF6D5C4Fu,
    0xFF6D7150u, 0xFF665270u, 0xFF496B67u, 0xFF785A54u,
    0xFF4E5C79u, 0xFF71624Au, 0xFF596A70u, 0xFF685F66u,
};
constexpr uint32_t kExitColor = 0xFF38B8F8u;

bool main_thread() {
    return g_ui != nullptr && g_ui->is_main_thread != nullptr && g_ui->is_main_thread() != 0;
}

void resolve_map_directory() {
    char package[256]{};
    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        const ssize_t count = read(fd, package, sizeof(package) - 1);
        close(fd);
        if (count > 0) package[static_cast<size_t>(count)] = '\0';
    }
    if (package[0] == '\0') std::snprintf(package, sizeof(package), "%s", kDefaultPackage);
    g_map_directory = "/sdcard/Android/data/";
    g_map_directory += package;
    g_map_directory += "/files/qol/resources/map";
}

bool components_complete() {
    return g_components != nullptr && g_components->scroll_list_create != nullptr &&
        g_components->scroll_list_process != nullptr && g_components->scroll_list_draw != nullptr &&
        g_components->scroll_list_get_row != nullptr && g_components->destroy != nullptr &&
        g_components->draw_control_text != nullptr &&
        g_components->draw_container_ornamented != nullptr &&
        g_components->dispatch_native_event != nullptr && g_components->nav_button_create != nullptr &&
        g_components->nav_button_draw != nullptr;
}

void draw_row(void*, uint32_t index, QolUiHandle row, uint8_t selected) {
    if (index >= g_maps.size() || g_ui == nullptr || g_ui->control_absolute_origin == nullptr ||
        g_ui->fill_rect == nullptr) return;
    int64_t x = 0, y = 0;
    if (!contracts::succeeded(g_ui->control_absolute_origin(row, &x, &y))) return;
    if (selected != 0) {
        (void)g_ui->fill_rect({x, y, kLeftView.w, kRowHeight}, contracts::ui_theme::kRowSelectedBg);
    }
    (void)g_ui->fill_rect({x, y + kRowHeight - 2, kLeftView.w, 2},
                          contracts::ui_theme::kFrameGold);
    char label[32]{};
    std::snprintf(label, sizeof(label), "m%u", g_maps[index].id);
    (void)g_components->draw_control_text(row, label, 16, 16,
        selected != 0 ? contracts::ui_theme::kTextGold : contracts::ui_theme::kTextNormal, 0, 1);
    char size[32]{};
    std::snprintf(size, sizeof(size), "%u×%u", g_maps[index].width, g_maps[index].height);
    (void)g_components->draw_control_text(row, size, 258, 16,
        contracts::ui_theme::kTextDisabled, 1, 0);
}

void on_select(void*, uint32_t index) {
    // List callbacks only request a selection; detail controls rebuild at process safe point.
    g_pending_selection = static_cast<int32_t>(index);
}

void resolve_map_name(uint32_t id) {
    g_map_name = "自定义";
    if (g_static_data == nullptr || g_static_data->map_name == nullptr || id > INT32_MAX) return;
    char buffer[256]{};
    uint32_t length = 0;
    if (contracts::succeeded(g_static_data->map_name(static_cast<int32_t>(id), buffer,
            sizeof(buffer), &length)) && length < sizeof(buffer)) {
        buffer[length] = '\0';
        if (length > 0) {
            g_map_name.assign(buffer, length);
            constexpr size_t kMaxNameBytes = 36;
            if (g_map_name.size() > kMaxNameBytes) {
                size_t end = kMaxNameBytes - 3;
                while (end > 0 && (static_cast<unsigned char>(g_map_name[end]) & 0xC0u) == 0x80u)
                    --end;
                g_map_name.resize(end);
                g_map_name += "…";
            }
        }
    }
}

uint32_t tile_color(uint16_t tile) {
    uint32_t hash = tile;
    hash ^= hash >> 7;
    hash *= 0x45d9f3bu;
    hash ^= hash >> 13;
    return kTilePalette[hash % kTilePalette.size()];
}

void rebuild_detail(int32_t index) {
    g_selected = index;
    g_detail = {};
    g_tile_colors.clear();
    g_exit_mask.clear();
    g_map_name.clear();
    g_map_directory.clear();
    g_status.clear();
    if (index < 0 || static_cast<size_t>(index) >= g_maps.size()) return;
    const auto& summary = g_maps[static_cast<size_t>(index)];
    if (!custom_map_read_detail(summary.id, &g_detail) ||
        g_detail.base_tiles.size() != static_cast<size_t>(summary.width) * summary.height) {
        g_status = "地图读取失败";
        return;
    }
    resolve_map_name(summary.id);
    g_tile_colors.reserve(g_detail.base_tiles.size());
    for (uint16_t tile : g_detail.base_tiles) g_tile_colors.push_back(tile_color(tile));
    g_exit_mask.assign(g_detail.base_tiles.size(), 0);
    for (const auto& exit : g_detail.exits) {
        if (exit.x < g_detail.width && exit.y < g_detail.height) {
            const size_t position = static_cast<size_t>(exit.y) * g_detail.width + exit.x;
            g_exit_mask[position] = 1;
        }
    }
    g_frame = 0;
}

void destroy_panel() {
    g_active = false;
    g_pending_selection = -1;
    g_selected = -1;
    g_pressed_list = nullptr;
    g_back_pressed = false;
    if (g_components != nullptr) {
        if (g_left != nullptr) (void)g_components->destroy(g_left);
        if (g_back != nullptr) (void)g_components->destroy(g_back);
    }
    if (g_ui != nullptr && g_root != nullptr && g_ui->delete_control != nullptr)
        (void)g_ui->delete_control(g_root);
    g_root = nullptr;
    g_left = nullptr;
    g_back = nullptr;
    g_maps.clear();
    g_detail = {};
    g_tile_colors.clear();
    g_exit_mask.clear();
    g_map_name.clear();
    g_status.clear();
}

void draw_map_detail() {
    if (g_selected < 0 || static_cast<size_t>(g_selected) >= g_maps.size()) {
        (void)g_components->draw_control_text(g_root, "请选择左侧地图", 640, 270,
            contracts::ui_theme::kTextDisabled, 2, 1);
        return;
    }
    const auto& summary = g_maps[static_cast<size_t>(g_selected)];
    char id[32]{};
    char size[48]{};
    std::snprintf(id, sizeof(id), "m%u", summary.id);
    std::snprintf(size, sizeof(size), "%u×%u", summary.width, summary.height);
    (void)g_components->draw_control_text(g_root, id, 368, 118,
        contracts::ui_theme::kTextGold, 0, 1);
    (void)g_components->draw_control_text(g_root, g_map_name.empty() ? "—" : g_map_name.c_str(),
        450, 118, contracts::ui_theme::kTextNormal, 0, 1);
    (void)g_components->draw_control_text(g_root, size, 920, 118,
        contracts::ui_theme::kTextDisabled, 1, 1);
    (void)g_ui->fill_rect({368, 152, 544, 1}, contracts::ui_theme::kFrameGold);
    if (!g_status.empty()) {
        (void)g_components->draw_control_text(g_root, g_status.c_str(), 640, 285,
            contracts::ui_theme::kError, 2, 1);
        return;
    }
    if (g_detail.width == 0 || g_detail.height == 0 || g_tile_colors.empty()) return;
    // Keep the expensive tile matrix cached on screen between redraws; selection resets g_frame.
    if ((g_frame & 3u) != 0) return;

    constexpr int64_t kGridX = 368;
    constexpr int64_t kGridY = 170;
    constexpr int64_t kGridW = 544;
    constexpr int64_t kGridH = 350;
    const uint32_t columns = g_detail.width;
    const uint32_t rows = g_detail.height;
    for (uint32_t y = 0; y < rows; ++y) {
        const int64_t top = kGridY + static_cast<int64_t>(y) * kGridH / rows;
        const int64_t bottom = kGridY + static_cast<int64_t>(y + 1) * kGridH / rows;
        for (uint32_t x = 0; x < columns; ++x) {
            const size_t index = static_cast<size_t>(y) * columns + x;
            const int64_t left = kGridX + static_cast<int64_t>(x) * kGridW / columns;
            const int64_t right = kGridX + static_cast<int64_t>(x + 1) * kGridW / columns;
            const uint32_t color = index < g_exit_mask.size() && g_exit_mask[index] != 0
                ? kExitColor : g_tile_colors[index];
            (void)g_ui->fill_rect({left, top, std::max<int64_t>(1, right - left - 1),
                                   std::max<int64_t>(1, bottom - top - 1)}, color);
        }
    }
    char dimensions[48]{};
    std::snprintf(dimensions, sizeof(dimensions), "%u×%u 瓦片  ·  金色格为出口",
                  summary.width, summary.height);
    (void)g_components->draw_control_text(g_root, dimensions, 640, 532,
        contracts::ui_theme::kTextDisabled, 2, 0);
}

}  // namespace

void custom_map_panel_bind(const contracts::QolHostSdk* sdk,
                           const contracts::QolUiComponents* components) {
    if (g_root != nullptr) destroy_panel();
    g_sdk = sdk;
    g_components = components;
    g_ui = sdk != nullptr ? sdk->ui : nullptr;
    g_static_data = nullptr;
    if (sdk != nullptr && sdk->capabilities.require != nullptr) {
        const void* iface = nullptr;
        if (contracts::succeeded(sdk->capabilities.require(contracts::kStaticDataCapabilityId,
                {contracts::kStaticDataVersion.major, contracts::kStaticDataVersion.minor}, &iface)))
            g_static_data = static_cast<const contracts::QolStaticDataApi*>(iface);
    }
}

bool custom_map_provider_open() {
    return enabled() && g_sdk != nullptr && g_components != nullptr;
}

void custom_map_provider_close() {
    if (g_active) destroy_panel();
}

bool custom_map_provider_active() { return g_active; }

const char* custom_map_provider_block_reason() {
    return enabled() ? nullptr : "自定义地图加载未启用";
}

void custom_map_panel_enter() {
    if (!main_thread() || !components_complete() || g_ui == nullptr || g_root != nullptr) return;
    if (g_ui->create_control == nullptr || g_ui->set_rect == nullptr ||
        g_ui->delete_control == nullptr || g_ui->control_absolute_origin == nullptr ||
        g_ui->fill_rect == nullptr || g_ui->begin_frame == nullptr || g_ui->end_frame == nullptr ||
        g_ui->hit_test == nullptr) return;
    g_maps.clear();
    resolve_map_directory();
    g_status = custom_map_scan(&g_maps) ? "" : "读取地图目录失败";
    if (g_maps.empty() && g_status.empty()) g_status = "未发现自定义地图文件";
    int32_t inset_x = 0, inset_y = 0;
    if (g_ui->screen_inset_width != nullptr) (void)g_ui->screen_inset_width(&inset_x);
    if (g_ui->screen_inset_height != nullptr) (void)g_ui->screen_inset_height(&inset_y);
    if (!contracts::succeeded(g_ui->create_control(0, nullptr, &g_root)) || g_root == nullptr) return;
    (void)g_ui->set_rect(g_root, {inset_x, inset_y, kPanelW, kPanelH});
    const contracts::UiNavButtonDesc nav{{contracts::ui_theme::kNavBoxAbsX - inset_x,
        contracts::ui_theme::kNavBoxAbsY - inset_y, contracts::ui_theme::kNavBoxW,
        contracts::ui_theme::kNavBoxH}, contracts::kUiNavButtonAuto, nullptr, nullptr};
    (void)g_components->nav_button_create(&nav, g_root, &g_back);
    if (g_ui->load_image_unit != nullptr) (void)g_ui->load_image_unit(0x0f);
    const contracts::UiScrollListDesc list{kLeftView, kRowHeight,
        static_cast<uint32_t>(g_maps.size()), g_maps.empty() ? -1 : 0,
        &draw_row, &on_select, nullptr};
    if (!contracts::succeeded(g_components->scroll_list_create(&list, g_root, &g_left))) {
        destroy_panel();
        return;
    }
    g_active = true;
    g_frame = 0;
    if (g_maps.empty()) g_selected = -1;
    else rebuild_detail(0);
}

void custom_map_panel_process() {
    if (!main_thread() || !g_active || g_root == nullptr) return;
    if (g_pending_selection >= 0) {
        const int32_t selected = g_pending_selection;
        g_pending_selection = -1;
        if (selected != g_selected) rebuild_detail(selected);
    }
    if (g_left != nullptr) (void)g_components->scroll_list_process(g_left);
    if (!contracts::succeeded(g_ui->begin_frame())) return;
    int64_t root_x = 0, root_y = 0;
    if (contracts::succeeded(g_ui->control_absolute_origin(g_root, &root_x, &root_y))) {
        const UiRect left{root_x + kLeftView.x - 8, root_y + kLeftView.y - 8,
                          kLeftView.w + 16, kLeftView.h + 16};
        const UiRect right{root_x + kMapView.x - 8, root_y + kMapView.y - 8,
                           kMapView.w + 16, kMapView.h + 16};
        (void)g_ui->fill_rect(left, contracts::ui_theme::kPanelBg);
        (void)g_components->draw_container_ornamented(left);
        (void)g_ui->fill_rect(right, contracts::ui_theme::kPanelBg);
        (void)g_components->draw_container_ornamented(right);
    }
    (void)g_components->draw_control_text(g_root, "自定义地图", 640, 54,
        contracts::ui_theme::kTextGold, 2, 2);
    if (g_left != nullptr) (void)g_components->scroll_list_draw(g_left);
    if (g_maps.empty()) {
        (void)g_components->draw_control_text(g_root, g_status.c_str(), 164, 270,
            contracts::ui_theme::kTextDisabled, 2, 1);
        (void)g_components->draw_control_text(g_root, g_map_directory.c_str(), 164, 304,
            contracts::ui_theme::kTextDisabled, 2, 0);
    } else {
        draw_map_detail();
    }
    if (g_back != nullptr) (void)g_components->nav_button_draw(g_back);
    (void)g_ui->end_frame();
    ++g_frame;
}

void custom_map_panel_f3() { if (main_thread()) destroy_panel(); }
void custom_map_panel_f4() {}

uint32_t custom_map_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (!main_thread() || !g_active || g_root == nullptr) return 1;
    if (event != 0x17 && event != 0x18 && event != 0x19) return 1;
    if (event == 0x19) {
        uint32_t consumed = 0;
        (void)g_components->dispatch_native_event(g_pressed_list != nullptr ? g_pressed_list : g_root,
            event, param, param2, &consumed);
        return consumed;
    }
    int64_t x = 0, y = 0;
    if (param == 0 || g_ui->touch_coordinates == nullptr ||
        !contracts::succeeded(g_ui->touch_coordinates(param, &x, &y))) return 1;
    if (g_back != nullptr) {
        const bool inside = g_ui->hit_test(g_back, x, y, {0, 0,
            contracts::ui_theme::kNavBoxW, contracts::ui_theme::kNavBoxH}) > 0;
        if ((event == 0x17 && inside) || (event == 0x18 && g_back_pressed)) {
            g_back_pressed = event == 0x17;
            uint32_t consumed = 0;
            (void)g_components->dispatch_native_event(g_root, event, param, param2, &consumed);
            return consumed;
        }
    }
    if (event == 0x17) g_pressed_list = g_left != nullptr &&
        g_ui->hit_test(g_left, x, y, {0, 0, kLeftView.w, kLeftView.h}) > 0 ? g_left : nullptr;
    if (g_pressed_list == nullptr) return 1;
    uint32_t consumed = 0;
    (void)g_components->dispatch_native_event(g_pressed_list, event, param, param2, &consumed);
    if (event == 0x18) g_pressed_list = nullptr;
    return 1;
}

}  // namespace qol::features::custom_map
