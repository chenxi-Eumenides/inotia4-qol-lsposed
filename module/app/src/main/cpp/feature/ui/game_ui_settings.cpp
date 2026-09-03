#include "game_ui_settings.h"

#include "game_access.h"
#include "game_ops_common.h"
#include "feature/patch/game_patch.h"
#include "game_ptr_hook.h"
#include "game_state.h"
#include "game_symbols.h"
#include "game_ui.h"
#include "game_ui_kit.h"
#include "game_ui_components.h"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#define SETTINGS_TAG "Inotia4UISettings"
#define SETTINGS_LOG(...) __android_log_print(ANDROID_LOG_INFO, SETTINGS_TAG, __VA_ARGS__)

#define POPUP_STATE_SIZE 0x40
#define CB_TEXT_SIZE 0x20

// 面板布局（逻辑坐标 0-960×0-640 空间，root 相对全屏居中；触摸坐标同空间）
#define ROOT_W 0x3c0
#define ROOT_H 0x280
#define CONTENT_W 0x430
#define CONTENT_X ((ROOT_W - CONTENT_W) / 2)
#define SETTINGS_ROW_COUNT 4
#define SETTINGS_COLUMN_COUNT 2
#define SETTINGS_GRID_ROWS ((SETTINGS_ROW_COUNT + SETTINGS_COLUMN_COUNT - 1) / SETTINGS_COLUMN_COUNT)
#define VISIBLE_GRID_ROWS 4
#define CELL_W (CONTENT_W / SETTINGS_COLUMN_COUNT)
#define CELL_H 0x66
#define GRID_Y 0x60
#define GRID_H (VISIBLE_GRID_ROWS * CELL_H)
#define ADDR_H 0x28
#define ROW_BTN_W 0xa8
#define ROW_BTN_H 0x28
#define CELL_PADDING 0x20
#define ADDR_Y 0x18
#define BACK_BTN_W 0x4a
#define BACK_BTN_H 0x51
#define UI_GOLD 0xFF00B4D7
#define UI_OPTION_TEXT 0xFFCB9EE2
#define TITLE_BACKGROUND_LEFT_UNIT 0x4f
#define TITLE_BACKGROUND_RIGHT_UNIT 0x50
#define TITLE_BACKGROUND_LOC 0x0

namespace {

std::mutex g_settings_mtx;
std::atomic<bool> g_thread_started{false};
jclass g_config_bridge_class = nullptr;
bool g_more_games_injected = false;
PtrHook g_more_games_hook;

bool g_panel_active = false;
uint8_t* g_state_entry = nullptr;
uint8_t g_state_backup[POPUP_STATE_SIZE] = {0};
int g_state_id = -1;
void* g_root = nullptr;
bool g_option_images_loaded = false;
bool g_title_background_images_loaded = false;
bool g_background_unavailable_logged = false;
int g_close_delay_frames = 0;
bool g_back_pressed = false;

struct SettingsRow {
    void* btn;
    void* desc;
};
SettingsRow g_rows[SETTINGS_ROW_COUNT];
void* g_back_btn = nullptr;
void* g_addr_desc = nullptr;

// 配置键名（与 Kotlin ModuleConfig 字段一致）
static const char* kRowKeys[SETTINGS_ROW_COUNT] = {"stackLimitIncrease", "moveMergeEnabled", "opEnabled", "extensionBagEnabled"};
static const char* kRowLabels[SETTINGS_ROW_COUNT] = {"堆叠上限", "拖拽合并", "OP能力", "扩展背包"};

// 当前配置值缓存（面板打开时从 Kotlin 拉取，切换时本地翻转+上抛）
static char g_row_status[SETTINGS_ROW_COUNT][CB_TEXT_SIZE] = {"关", "关", "关"};
static char g_addr_text[CB_TEXT_SIZE] = "";

bool inject_state_entry_locked();
void settings_row_clicked(void* ctrl);
void settings_back_clicked(void* ctrl);

#include "game_ui_settings_geometry.inc"
#include "game_ui_settings_config.inc"
#include "game_ui_settings_render.inc"
#include "game_ui_settings_panel.inc"
#include "game_ui_settings_injection.inc"
}  // namespace
#include "game_ui_settings_api.inc"
