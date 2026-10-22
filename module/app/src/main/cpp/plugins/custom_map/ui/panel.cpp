#include "panel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <map>
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
constexpr size_t kMaxVisibleMaps = 1024;
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
struct MapRow {
    CustomMapFile file;
    CustomMapSummary summary{};
    bool parsed = false;
    bool failed = false;
};
struct DetailCache {
    CustomMapFile file;
    CustomMapDetail detail;
    std::vector<uint32_t> tile_colors;
    std::vector<uint8_t> exit_mask;
};
std::vector<CustomMapFile> g_scan_files;
std::vector<MapRow> g_maps;
std::map<uint32_t, DetailCache> g_detail_cache;
const DetailCache* g_selected_detail = nullptr;
size_t g_scan_index = 0;
size_t g_scan_completed = 0;
size_t g_scan_failed = 0;
bool g_scan_available = false;
bool g_scan_truncated = false;
bool g_detail_loading = false;
bool g_skip_parse_frame = false;
std::string g_detail_status;
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
    std::snprintf(label, sizeof(label), "m%u", g_maps[index].file.id);
    (void)g_components->draw_control_text(row, label, 16, 16,
        selected != 0 ? contracts::ui_theme::kTextGold : contracts::ui_theme::kTextNormal, 0, 1);
    char size[32]{};
    if (g_maps[index].failed) {
        std::snprintf(size, sizeof(size), "读取失败");
    } else {
        std::snprintf(size, sizeof(size), "%u×%u", g_maps[index].summary.width,
                      g_maps[index].summary.height);
    }
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

bool same_file_snapshot(const CustomMapFile& left, const CustomMapFile& right) {
    return left.id == right.id && left.path == right.path && left.size == right.size &&
        left.modified_sec == right.modified_sec && left.modified_nsec == right.modified_nsec;
}

void request_detail(int32_t index) {
    g_selected = index;
    g_selected_detail = nullptr;
    g_detail_loading = false;
    g_skip_parse_frame = false;
    g_detail_status.clear();
    g_map_name.clear();
    g_frame = 0;
    if (index < 0 || static_cast<size_t>(index) >= g_maps.size()) return;
    const auto& row = g_maps[static_cast<size_t>(index)];
    if (row.failed) {
        g_detail_status = "地图文件头解析失败";
        return;
    }
    resolve_map_name(row.file.id);
    if (!custom_map_file_unchanged(row.file)) {
        g_detail_status = "文件已更改，请重新打开面板";
        return;
    }
    const auto cached = g_detail_cache.find(row.file.id);
    if (cached != g_detail_cache.end() && same_file_snapshot(cached->second.file, row.file)) {
        g_selected_detail = &cached->second;
        return;
    }
    g_detail_loading = true;
    g_detail_status = "地图示意图加载中…";
}

void rebuild_left_list() {
    if (g_left != nullptr) (void)g_components->destroy(g_left);
    g_left = nullptr;
    g_pressed_list = nullptr;
    if (g_maps.empty()) return;
    const int32_t selected = g_selected >= 0 && static_cast<size_t>(g_selected) < g_maps.size()
        ? g_selected : -1;
    const contracts::UiScrollListDesc list{kLeftView, kRowHeight,
        static_cast<uint32_t>(g_maps.size()), selected, &draw_row, &on_select, nullptr};
    if (!contracts::succeeded(g_components->scroll_list_create(&list, g_root, &g_left)))
        g_status = "地图列表刷新失败";
}

void process_one_header() {
    if (!g_scan_available || g_scan_index >= g_scan_files.size()) return;
    const CustomMapFile file = g_scan_files[g_scan_index++];
    MapRow row{};
    row.file = file;
    row.parsed = custom_map_read_summary(file, &row.summary);
    row.failed = !row.parsed;
    ++g_scan_completed;
    if (row.failed) ++g_scan_failed;
    g_maps.push_back(std::move(row));
    if (g_selected < 0) {
        g_selected = 0;
        request_detail(g_selected);
    }
    rebuild_left_list();
}

void process_detail_load() {
    if (!g_detail_loading || g_selected < 0 || static_cast<size_t>(g_selected) >= g_maps.size()) return;
    const MapRow& row = g_maps[static_cast<size_t>(g_selected)];
    DetailCache loaded{};
    loaded.file = row.file;
    if (!custom_map_read_detail(row.file, &loaded.detail) ||
        loaded.detail.width != row.summary.width || loaded.detail.height != row.summary.height ||
        loaded.detail.base_tiles.size() != static_cast<size_t>(row.summary.width) * row.summary.height) {
        g_detail_loading = false;
        g_detail_status = "地图详情读取失败或文件已更改";
        return;
    }
    loaded.tile_colors.reserve(loaded.detail.base_tiles.size());
    for (uint16_t tile : loaded.detail.base_tiles) loaded.tile_colors.push_back(tile_color(tile));
    loaded.exit_mask.assign(loaded.detail.base_tiles.size(), 0);
    for (const auto& exit : loaded.detail.exits) {
        if (exit.x < loaded.detail.width && exit.y < loaded.detail.height) {
            const size_t position = static_cast<size_t>(exit.y) * loaded.detail.width + exit.x;
            loaded.exit_mask[position] = 1;
        }
    }
    const uint32_t id = loaded.detail.id;
    auto result = g_detail_cache.insert_or_assign(id, std::move(loaded));
    g_detail_loading = false;
    g_selected_detail = &result.first->second;
    g_detail_status.clear();
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
    g_scan_files.clear();
    g_maps.clear();
    g_detail_cache.clear();
    g_selected_detail = nullptr;
    g_scan_index = 0;
    g_scan_completed = 0;
    g_scan_failed = 0;
    g_scan_available = false;
    g_scan_truncated = false;
    g_detail_loading = false;
    g_detail_status.clear();
    g_map_name.clear();
    g_status.clear();
}

void draw_map_detail() {
    if (g_selected < 0 || static_cast<size_t>(g_selected) >= g_maps.size()) {
        (void)g_components->draw_control_text(g_root, "请选择左侧地图", 640, 270,
            contracts::ui_theme::kTextDisabled, 2, 1);
        return;
    }
    const auto& row = g_maps[static_cast<size_t>(g_selected)];
    char id[32]{};
    char size[48]{};
    std::snprintf(id, sizeof(id), "m%u", row.file.id);
    if (row.parsed) {
        std::snprintf(size, sizeof(size), "%u×%u", row.summary.width, row.summary.height);
    } else {
        std::snprintf(size, sizeof(size), "—");
    }
    (void)g_components->draw_control_text(g_root, id, 368, 118,
        contracts::ui_theme::kTextGold, 0, 1);
    (void)g_components->draw_control_text(g_root, g_map_name.empty() ? "—" : g_map_name.c_str(),
        450, 118, contracts::ui_theme::kTextNormal, 0, 1);
    (void)g_components->draw_control_text(g_root, size, 920, 118,
        contracts::ui_theme::kTextDisabled, 1, 1);
    (void)g_ui->fill_rect({368, 152, 544, 1}, contracts::ui_theme::kFrameGold);
    if (g_selected_detail == nullptr) {
        const uint32_t color = g_detail_status.empty() ? contracts::ui_theme::kTextDisabled
                                                       : contracts::ui_theme::kError;
        (void)g_components->draw_control_text(g_root,
            g_detail_status.empty() ? "等待地图头部解析" : g_detail_status.c_str(), 640, 285,
            color, 2, 1);
        return;
    }
    const auto& detail = *g_selected_detail;
    if (detail.detail.width == 0 || detail.detail.height == 0 || detail.tile_colors.empty()) return;
    // Keep the expensive tile matrix cached on screen between redraws; selection resets g_frame.
    if ((g_frame & 3u) != 0) return;

    constexpr int64_t kGridX = 368;
    constexpr int64_t kGridY = 170;
    constexpr int64_t kGridW = 544;
    constexpr int64_t kGridH = 350;
    const uint32_t columns = detail.detail.width;
    const uint32_t rows = detail.detail.height;
    for (uint32_t y = 0; y < rows; ++y) {
        const int64_t top = kGridY + static_cast<int64_t>(y) * kGridH / rows;
        const int64_t bottom = kGridY + static_cast<int64_t>(y + 1) * kGridH / rows;
        for (uint32_t x = 0; x < columns; ++x) {
            const size_t index = static_cast<size_t>(y) * columns + x;
            const int64_t left = kGridX + static_cast<int64_t>(x) * kGridW / columns;
            const int64_t right = kGridX + static_cast<int64_t>(x + 1) * kGridW / columns;
            const uint32_t color = index < detail.exit_mask.size() && detail.exit_mask[index] != 0
                ? kExitColor : detail.tile_colors[index];
            (void)g_ui->fill_rect({left, top, std::max<int64_t>(1, right - left - 1),
                                   std::max<int64_t>(1, bottom - top - 1)}, color);
        }
    }
    char dimensions[48]{};
    std::snprintf(dimensions, sizeof(dimensions), "%u×%u 瓦片  ·  金色格为出口",
                  row.summary.width, row.summary.height);
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
    if (!main_thread() || !components_complete() || g_ui == nullptr) return;
    if (g_root != nullptr) destroy_panel();
    if (g_ui->create_control == nullptr || g_ui->set_rect == nullptr ||
        g_ui->delete_control == nullptr || g_ui->control_absolute_origin == nullptr ||
        g_ui->fill_rect == nullptr || g_ui->begin_frame == nullptr || g_ui->end_frame == nullptr ||
        g_ui->hit_test == nullptr) return;
    g_scan_files.clear();
    g_maps.clear();
    g_detail_cache.clear();
    g_selected_detail = nullptr;
    g_scan_index = 0;
    g_scan_completed = 0;
    g_scan_failed = 0;
    g_selected = -1;
    g_pending_selection = -1;
    g_detail_loading = false;
    g_detail_status.clear();
    g_status.clear();
    resolve_map_directory();
    g_scan_available = custom_map_list_files(&g_scan_files);
    if (g_scan_files.size() > kMaxVisibleMaps) {
        g_scan_files.resize(kMaxVisibleMaps);
        g_scan_truncated = true;
    } else {
        g_scan_truncated = false;
    }
    if (!g_scan_available) g_status = "地图目录读取失败";
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
    rebuild_left_list();
    if (!g_maps.empty() && g_left == nullptr) { destroy_panel(); return; }
    g_active = true;
    g_frame = 0;
    g_skip_parse_frame = true;
}

void custom_map_panel_process() {
    if (!main_thread() || !g_active || g_root == nullptr) return;
    if (g_pending_selection >= 0) {
        const int32_t selected = g_pending_selection;
        g_pending_selection = -1;
        if (selected != g_selected) request_detail(selected);
    }
    if (g_skip_parse_frame) g_skip_parse_frame = false;
    else if (g_detail_loading) process_detail_load();
    else process_one_header();
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
    if (!g_status.empty()) {
        (void)g_components->draw_control_text(g_root, g_status.c_str(), 640, 82,
            contracts::ui_theme::kError, 2, 0);
    } else if (g_scan_available && !g_scan_files.empty()) {
        char progress[80]{};
        if (g_scan_completed < g_scan_files.size()) {
            std::snprintf(progress, sizeof(progress), "解析中 %zu/%zu",
                          g_scan_completed, g_scan_files.size());
        } else {
            std::snprintf(progress, sizeof(progress), "解析完成 %zu/%zu%s%s",
                g_scan_completed, g_scan_files.size(),
                g_scan_failed == 0 ? "" : "（部分文件读取失败）",
                g_scan_truncated ? "，仅显示前1024项" : "");
        }
        (void)g_components->draw_control_text(g_root, progress, 640, 82,
            g_scan_failed == 0 ? contracts::ui_theme::kTextDisabled : contracts::ui_theme::kError,
            2, 0);
    }
    if (g_left != nullptr) (void)g_components->scroll_list_draw(g_left);
    if (!g_scan_available || g_scan_files.empty()) {
        (void)g_components->draw_control_text(g_root,
            g_scan_available ? "未发现自定义地图文件" : g_status.c_str(), 164, 270,
            contracts::ui_theme::kTextDisabled, 2, 1);
        (void)g_components->draw_control_text(g_root, g_map_directory.c_str(), 164, 304,
            contracts::ui_theme::kTextDisabled, 2, 0);
    } else if (g_maps.empty()) {
        (void)g_components->draw_control_text(g_root, "正在解析地图文件头…", 640, 285,
            contracts::ui_theme::kTextDisabled, 2, 1);
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
