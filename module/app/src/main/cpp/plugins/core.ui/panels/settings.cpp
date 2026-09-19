#include "plugins/core.ui/panels/settings.h"

#include "contracts/panels.h"
#include "core/native/game_ops_common.h"
#include "core/native/qol_log.h"
#include "game_access.h"
#include "game_ptr_hook.h"
#include "game_state.h"
#include "game_ui.h"
#include "plugins/core.ui/panels/rules.h"
#include "plugins/core.ui/panels/stack.h"
#include "plugins/core.ui/ui_components.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#define SETTINGS_LOG(...) QOL_LOG_INFO(QolDomain::kUi, __VA_ARGS__)

#define CB_TEXT_SIZE 0x20

// 面板布局（逻辑坐标 0-960×0-640 空间，root 相对全屏居中；触摸坐标同空间）
#define ROOT_W 0x3c0
#define ROOT_H 0x280
#define SETTINGS_COLUMN_COUNT panels_rules::kColumnCount
#define VISIBLE_GRID_ROWS panels_rules::kVisibleRows
#define SETTINGS_PAGE_CAPACITY panels_rules::kPageCapacity
#define PAGE_BUTTON_W 0x48
#define PAGE_BUTTON_H 0x48
#define PAGE_NAV_TEXT_GAP 0x28
// 保持分页前的原始网格宽度；2×4 后将翻页按钮移到网格下方，避免与网格重叠。
#define CONTENT_W 0x430
#define CONTENT_X ((ROOT_W - CONTENT_W) / 2)
#define CELL_W (CONTENT_W / SETTINGS_COLUMN_COUNT)
#define CELL_H 0x66
#define GRID_Y 0x60
#define GRID_H (VISIBLE_GRID_ROWS * CELL_H)
#define PAGE_INDICATOR_H 0x20
#define PAGE_NAV_Y (GRID_Y + GRID_H + 0x08)
#define PAGE_INDICATOR_Y (PAGE_NAV_Y + (PAGE_BUTTON_H - PAGE_INDICATOR_H) / 2)
#define HEADER_H 0x28
#define ROW_BTN_W 0xa8
#define ROW_BTN_H 0x28
// 存档备份格：按钮复用原版物品详情「使用」贴图（UIDesc_DrawMenuButton + loc 0x0e）。
// 该分片 149x75，现有绘制 API 只能按原始尺寸画；控制矩形高取开关同高 0x28，
// 宽度按贴图宽高比 149:75 同比得 0x50≈80（比其它开关窄）。
#define SAVEBACKUP_BTN_W 0x50
// 原版「使用」按钮分片：ButtonImgGroup(DrawID=0) 图组内 loc 0x0e
// （UIEquip_SetDescMenu 反汇编实证：使用/确认使用/佣兵徽章/骰子/开箱/解封共用）。
#define USE_BUTTON_LOC 0x0e
#define CELL_PADDING 0x20
#define HEADER_Y 0x18
#define BACK_BTN_W 0x4a
#define BACK_BTN_H 0x51
#define UI_GOLD 0xFF00B4D7
#define UI_OPTION_TEXT 0xFFCB9EE2
#define TITLE_BACKGROUND_LEFT_UNIT 0x4f
#define TITLE_BACKGROUND_RIGHT_UNIT 0x50
#define TITLE_BACKGROUND_LOC 0x0
// UIOption 的语言切换箭头：与原版 options 共用图像单元及箭头分片。
#define OPTION_IMAGE_UNIT 0x59
#define PAGE_PREV_ARROW_NORMAL_LOC 0x09
#define PAGE_PREV_ARROW_PRESSED_LOC 0x0a
#define PAGE_NEXT_ARROW_NORMAL_LOC 0x0b
#define PAGE_NEXT_ARROW_PRESSED_LOC 0x0c

namespace qol::features::core_ui {
namespace {

std::mutex g_settings_mtx;
const contracts::QolHostSdk* g_sdk = nullptr;
jclass g_config_bridge_class = nullptr;
bool g_more_games_injected = false;
PtrHook g_more_games_hook;
// 按钮绑定状态（主线程帧任务写；fill_status/HTTP 只读）：
// 0=waiting 1=live 2=stale 3=error。原子量避免 HTTP 线程读游戏内存。
std::atomic<int32_t> g_more_games_bind{0};
std::atomic<bool> g_more_games_error{false};

bool g_panel_active = false;
int g_state_id = -1;
void* g_root = nullptr;
bool g_option_images_loaded = false;
bool g_title_background_images_loaded = false;
bool g_background_unavailable_logged = false;
bool g_page_arrow_unavailable_logged = false;
int g_close_delay_frames = 0;
bool g_back_pressed = false;

// own:popup.settings 的 PopupState 占用条目（资源事务 acquire 写入、release 写回）。
contracts::QolPopupStateEntry g_popup_entry{};
bool g_popup_acquired = false;

struct SettingsRow {
    void* btn;
    void* desc;
};

// 配置项唯一登记表：key、label、page。
// page 是逻辑页码；空出的逻辑页会在运行时按升序压缩，新增项只需在此表增加一行。
// 空 key 的项是非开关入口，当前用于存档备份。
using SettingsItem = panels_rules::Item;

static const SettingsItem kSettingsItems[] = {
    {"", "存档备份", 1},
    {"extensionBagEnabled", "扩展背包", 1},
    {"stackLimitIncrease", "堆叠上限", 1},
    {"apiEnabled", "API服务", 1},
    {"autoSellEnabled", "自动出售", 1},
    {"simpleModeEnabled", "简单模式", 1},
    {"moveMergeEnabled", "拖拽合并", 2},
    {"customRecipeEnabled", "新合成系统", 2},
    {"pushAllEnabled", "推怪", 2},
    {"debugLogEnabled", "调试日志", 2},
    {"opEnabled", "OP能力", 3},
};
static constexpr int kSettingsItemCount =
    static_cast<int>(sizeof(kSettingsItems) / sizeof(kSettingsItems[0]));

SettingsRow g_rows[kSettingsItemCount];
void* g_prev_page_btn = nullptr;
void* g_next_page_btn = nullptr;
void* g_back_btn = nullptr;
void* g_header_label = nullptr;
int g_page_index = 0;
int g_total_pages = 1;

// 当前配置值缓存（面板打开时从 Kotlin 拉取，切换时本地翻转+上抛）
static char g_row_status[kSettingsItemCount][CB_TEXT_SIZE] = {
    "", "关", "关", "开", "关", "关", "关", "关", "关", "关", "关"
};
static char g_header_text[CB_TEXT_SIZE] = "QoL模块";

void settings_row_clicked(void* ctrl);
void settings_page_clicked(void* ctrl);
void settings_back_clicked(void* ctrl);
void settings_savebackup_clicked(void* ctrl);  // v0.7.x：存档备份按钮
void savebackup_btn_draw(void* ctrl);
void page_btn_draw(void* ctrl);

#include "plugins/core.ui/panels/settings_geometry.inc"
#include "plugins/core.ui/panels/settings_config.inc"
#include "plugins/core.ui/panels/settings_render.inc"
#include "plugins/core.ui/panels/settings_panel.inc"
#include "plugins/core.ui/panels/settings_injection.inc"
}  // namespace

void settings_bind_sdk(const contracts::QolHostSdk* sdk) { g_sdk = sdk; }

// ---- own:popup.settings（kOwnership）：整段占用 F_PANEL_UNK1_ENTER 的 5 回调 ----

int32_t settings_popup_acquire(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_popup_entry.slot != nullptr) return 0;
    if (g_sdk == nullptr || g_sdk->hooks.popup_resolve == nullptr ||
        g_sdk->hooks.popup_acquire == nullptr) {
        SETTINGS_LOG("popup.settings: hooks API not ready");
        return 1;
    }
    contracts::QolPopupStateEntry entry{};
    if (!contracts::succeeded(g_sdk->hooks.popup_resolve("F_PANEL_UNK1_ENTER", &entry))) {
        SETTINGS_LOG("popup.settings: state entry (INAP_GEMSHOP) not found");
        return 1;
    }
    const contracts::QolPopupStateHooks popup_hooks{
        &settings_panel_enter,
        &settings_panel_process,
        &settings_panel_f3,
        &settings_panel_f4,
        &settings_panel_event,
    };
    if (!contracts::succeeded(g_sdk->hooks.popup_acquire(&entry, &popup_hooks))) {
        SETTINGS_LOG("popup.settings: acquire rejected");
        return 1;
    }
    g_popup_entry = entry;
    g_state_id = entry.state_id;
    g_popup_acquired = true;
    SETTINGS_LOG("popup.settings acquired: state id=%d", g_state_id);
    return 0;
}

void settings_popup_release(const char* id) {
    (void)id;
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_popup_entry.slot != nullptr && g_sdk != nullptr &&
        g_sdk->hooks.popup_restore != nullptr) {
        (void)g_sdk->hooks.popup_restore(&g_popup_entry);
    }
    g_popup_acquired = false;
    g_state_id = -1;
    SETTINGS_LOG("popup.settings released");
}

// ---- PanelProviderSpec ----

bool settings_provider_open() {
    int state_id = -1;
    {
        std::lock_guard<std::mutex> lock(g_settings_mtx);
        if (g_base == 0 || g_state_id < 0) return false;
        state_id = g_state_id;
    }
    if (fn_ui_set_popup_process_info == nullptr) return false;
    // 锁外调游戏 pop 入口：push 链可能同步回调面板 enter/process。
    fn_ui_set_popup_process_info(1, state_id);
    SETTINGS_LOG("open settings panel state id=%d", state_id);
    return true;
}

void settings_provider_close() {
    // 幂等：未激活时不触碰游戏 UI（避免栈对陈旧栈顶调用 close 时误关当前顶层面板）。
    if (!settings_provider_active()) return;
    settings_ui_close_panel();
    if (fn_ui_set_popup_process_info != nullptr) fn_ui_set_popup_process_info(3, 0);
}

bool settings_provider_active() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    return g_panel_active;
}

const char* settings_provider_block_reason() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    return g_panel_active ? "ui occupied: settings panel" : nullptr;
}

// 关闭设置面板：只清本模块 panel 状态；调用方负责 fn_ui_set_popup_process_info(3, 0)
// 让游戏主循环关闭 UI（provider.close 已合并该步）。
void settings_ui_close_panel() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_panel_active) {
        settings_panel_f3();
        SETTINGS_LOG("settings panel closed by external caller");
    }
}

// ---- 对外 API（原 data_settings_ui_*）----

void settings_register_config_bridge(JNIEnv* env, jclass bridge_class) {
    if (env == nullptr || bridge_class == nullptr) return;
    if (g_config_bridge_class != nullptr) {
        env->DeleteGlobalRef(g_config_bridge_class);
    }
    g_config_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge_class));
}

std::string data_settings_ui_inject() {
    if (settings_popup_acquire(nullptr) != 0) return op_err("state entry inject failed");
    if (!settings_try_bind_more_games()) {
        // 控件实例通常在启动期尚未创建：不视为失败，帧任务会在主菜单重试。
        SETTINGS_LOG("settings more games entry not ready; frame task will retry");
    }
    return op_ok();
}

std::string data_settings_ui_status_json() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    void* btn = more_games_btn();
    int64_t bx = -1, by = -1, bw = -1, bh = -1;
    if (btn != nullptr) {
        uint8_t* co = reinterpret_cast<uint8_t*>(btn);
        bx = *reinterpret_cast<int64_t*>(co + CO_RECT_X);
        by = *reinterpret_cast<int64_t*>(co + CO_RECT_Y);
        bw = *reinterpret_cast<int64_t*>(co + CO_RECT_W);
        bh = *reinterpret_cast<int64_t*>(co + CO_RECT_H);
    }
    char buf[640];
    snprintf(buf, sizeof(buf),
        "{"
        "\"more_games_injected\":%s,\"more_games_btn\":\"%p\","
        "\"more_games_rect\":[%lld,%lld,%lld,%lld],"
        "\"more_games_bind\":\"%s\","
        "\"state_id\":%d,\"state_injected\":%s,"
        "\"panel_active\":%s,\"root\":\"%p\","
        "\"stack_limit_enabled\":%s,"
        "\"screen\":\"%s\"}",
        g_more_games_injected ? "true" : "false", btn,
        static_cast<long long>(bx), static_cast<long long>(by),
        static_cast<long long>(bw), static_cast<long long>(bh),
        settings_more_games_bind_state(),
        g_state_id, g_popup_acquired ? "true" : "false",
        g_panel_active ? "true" : "false", g_root,
        (g_sdk != nullptr && g_sdk->config.get_bool != nullptr && [&]() { int32_t enabled = 0; (void)g_sdk->config.get_bool("plugin.stack_limit.enabled", 0, &enabled); return enabled != 0; }()) ? "true" : "false",
        data_ui_screen());
    return std::string(buf);
}

std::string data_settings_ui_restore() {
    settings_ui_close_panel();
    {
        std::lock_guard<std::mutex> lock(g_settings_mtx);
        g_page_index = 0;
        settings_sync_page_state();
        if (g_option_images_loaded) {
            ui_original::unload_option_images();
            g_option_images_loaded = false;
        }
    }
    settings_release_more_games();
    settings_popup_release(nullptr);
    return op_ok();
}

std::string data_settings_ui_open_panel() {
    if (ui_panels_open("settings")) return op_ok();
    return op_err("state not injected");
}

std::string data_settings_ui_open_option() {
    if (g_base == 0) return op_err("libgame not ready");
    if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");
    if (g_sdk == nullptr || g_sdk->hooks.popup_resolve == nullptr) {
        return op_err("hooks API unavailable");
    }
    contracts::QolPopupStateEntry entry{};
    if (!contracts::succeeded(g_sdk->hooks.popup_resolve("F_PANEL_OPTIONS_ENTER", &entry))) {
        return op_err("option state not found");
    }
    fn_ui_set_popup_process_info(1, entry.state_id);
    SETTINGS_LOG("open option state id=%d", entry.state_id);
    return op_ok();
}

// ---- 「更多游戏」按钮绑定（帧任务驱动；运行时控件实例槽）----

void settings_release_more_games() {
    // 仅当槽仍可访问且仍指向 wrapper 时写回原值；对象已销毁则只丢弃句柄，不解引用旧地址。
    if (g_more_games_hook.slot != nullptr &&
        game_memory_accessible(g_more_games_hook.slot, sizeof(void*), 'r') &&
        *g_more_games_hook.slot == reinterpret_cast<void*>(&more_games_clicked)) {
        *g_more_games_hook.slot = g_more_games_hook.orig;
    }
    g_more_games_hook.slot = nullptr;
    g_more_games_hook.orig = nullptr;
    g_more_games_injected = false;
    g_more_games_bind.store(0, std::memory_order_relaxed);
    g_more_games_error.store(false, std::memory_order_relaxed);
}

const char* settings_more_games_bind_state() {
    if (g_more_games_error.load(std::memory_order_relaxed)) return "error";
    if (!g_more_games_injected) return "waiting";
    // 只读判断：主线程帧任务写入的原子标志 + 句柄；此处可能被 HTTP 线程调用，
    // 只读取已发布状态，不再解引用游戏内存。
    return g_more_games_bind.load(std::memory_order_relaxed) == 1 ? "live" : "stale";
}

void settings_more_games_tick() {
    const char* screen = data_ui_screen();
    const bool on_main_menu = screen != nullptr && strcmp(screen, "main_menu") == 0;
    if (!on_main_menu) {
        if (g_more_games_injected) {
            g_more_games_bind.store(1, std::memory_order_relaxed);
            g_more_games_error.store(false, std::memory_order_relaxed);
        }
        return;
    }
    if (settings_try_bind_more_games()) {
        g_more_games_bind.store(1, std::memory_order_relaxed);
        g_more_games_error.store(false, std::memory_order_relaxed);
        return;
    }
    // 主菜单且绑定失败：控件尚未解析 → waiting；控件可解析但仍失败 → error。
    g_more_games_error.store(more_games_btn() != nullptr, std::memory_order_relaxed);
    g_more_games_bind.store(0, std::memory_order_relaxed);
}

}  // namespace qol::features::core_ui
