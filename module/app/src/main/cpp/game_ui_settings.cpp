#include "game_ui_settings.h"

#include "game_access.h"
#include "game_ops_common.h"
#include "game_patch.h"
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

UiRect settings_root_rect() {
    const int64_t screen_w = ROOT_W + static_cast<int64_t>(fn_calc_res_width()) * 2;
    const int64_t screen_h = ROOT_H + static_cast<int64_t>(fn_calc_res_height()) * 2;
    if (screen_w <= 0 || screen_h <= 0) return {0, 0, ROOT_W, ROOT_H};
    const int64_t logical_w = screen_w * ROOT_H / screen_h;
    return {(logical_w - ROOT_W) / 2, 0, ROOT_W, ROOT_H};
}

UiRect settings_row_rect(int index) {
    const int column = index % SETTINGS_COLUMN_COUNT;
    const int row = index / SETTINGS_COLUMN_COUNT;
    return {CONTENT_X + column * CELL_W, GRID_Y + row * CELL_H, CELL_W, CELL_H};
}

UiRect settings_label_rect(int index) {
    UiRect cell = settings_row_rect(index);
    constexpr int64_t label_h = 0x28;
    return {cell.x + CELL_PADDING, cell.y + (cell.h - label_h) / 2,
            cell.w - ROW_BTN_W - CELL_PADDING * 3, label_h};
}

UiRect settings_toggle_rect(int index) {
    UiRect cell = settings_row_rect(index);
    return {cell.x + cell.w - ROW_BTN_W - CELL_PADDING,
            cell.y + (cell.h - ROW_BTN_H) / 2, ROW_BTN_W, ROW_BTN_H};
}

// ---- Kotlin 配置桥接（native 反调 ModuleConfigUiBridge）----

bool jni_get_config_json(char* out, size_t out_size) {
    if (g_base == 0) return false;
    JavaVM* jvm = g_jvm();
    if (jvm == nullptr) return false;
    JNIEnv* env = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
    }
    jclass cls = g_config_bridge_class;
    if (cls == nullptr) cls = env->FindClass("com/inotia4/export/ModuleConfigUiBridge");
    if (cls == nullptr) { env->ExceptionClear(); return false; }
    jmethodID mid = env->GetStaticMethodID(cls, "getConfigJson", "()Ljava/lang/String;");
    if (mid == nullptr) {
        env->ExceptionClear();
        if (g_config_bridge_class == nullptr) env->DeleteLocalRef(cls);
        return false;
    }
    jstring s = static_cast<jstring>(env->CallStaticObjectMethod(cls, mid));
    if (g_config_bridge_class == nullptr) env->DeleteLocalRef(cls);
    if (s == nullptr) { env->ExceptionClear(); return false; }
    const char* utf = env->GetStringUTFChars(s, nullptr);
    if (utf == nullptr) { env->DeleteLocalRef(s); return false; }
    snprintf(out, out_size, "%s", utf);
    env->ReleaseStringUTFChars(s, utf);
    env->DeleteLocalRef(s);
    return true;
}

bool jni_toggle_config(const char* key) {
    if (g_base == 0) return false;
    JavaVM* jvm = g_jvm();
    if (jvm == nullptr) return false;
    JNIEnv* env = nullptr;
    if (jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
    }
    jclass cls = g_config_bridge_class;
    if (cls == nullptr) cls = env->FindClass("com/inotia4/export/ModuleConfigUiBridge");
    if (cls == nullptr) { env->ExceptionClear(); return false; }
    jmethodID mid = env->GetStaticMethodID(cls, "toggleConfig", "(Ljava/lang/String;)Ljava/lang/String;");
    if (mid == nullptr) {
        env->ExceptionClear();
        if (g_config_bridge_class == nullptr) env->DeleteLocalRef(cls);
        return false;
    }
    jstring jk = env->NewStringUTF(key);
    jstring s = static_cast<jstring>(env->CallStaticObjectMethod(cls, mid, jk));
    env->DeleteLocalRef(jk);
    if (g_config_bridge_class == nullptr) env->DeleteLocalRef(cls);
    bool ok = false;
    if (s != nullptr) {
        const char* utf = env->GetStringUTFChars(s, nullptr);
        if (utf != nullptr) {
            ok = (strcmp(utf, "ok") == 0);
            SETTINGS_LOG("toggle %s -> %s", key, utf);
            env->ReleaseStringUTFChars(s, utf);
        }
        env->DeleteLocalRef(s);
    }
    env->ExceptionClear();
    return ok;
}

void parse_config_into_rows(const char* json) {
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
        const char* p = strstr(json, kRowKeys[i]);
        if (p == nullptr) continue;
        p = strchr(p, ':');
        if (p == nullptr) continue;
        ++p;
        while (*p == ' ' || *p == '\t') ++p;
        snprintf(g_row_status[i], CB_TEXT_SIZE, "%s", (*p == 't' || *p == '1') ? "开" : "关");
    }
    const char* addr = strstr(json, "listenAddress");
    if (addr != nullptr) {
        const char* colon = strchr(addr, ':');
        if (colon != nullptr) {
            const char* q1 = strchr(colon, '"');
            if (q1 != nullptr) {
                const char* q2 = strchr(q1 + 1, '"');
                if (q2 != nullptr) {
                    size_t len = static_cast<size_t>(q2 - q1 - 1);
                    if (len >= CB_TEXT_SIZE) len = CB_TEXT_SIZE - 1;
                    memcpy(g_addr_text, q1 + 1, len);
                    g_addr_text[len] = 0;
                }
            }
        }
    }
    const char* port = strstr(json, "listenPort");
    if (port != nullptr) {
        const char* colon = strchr(port, ':');
        if (colon != nullptr) {
            char* end = nullptr;
            long v = strtol(colon + 1, &end, 10);
            if (end != nullptr && end != colon + 1) {
                snprintf(g_addr_text + strlen(g_addr_text), CB_TEXT_SIZE - strlen(g_addr_text),
                         ":%ld", v);
            }
        }
    }
}

// ---- 面板绘制（DrawProc：色块 + 文字）----

void row_btn_draw(void* ctrl) {
    if (g_base == 0) return;
    bool enabled = false;
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
        if (ctrl != g_rows[i].btn) continue;
        enabled = strcmp(g_row_status[i], "开") == 0;
        break;
    }
    if (!ui_original::draw_toggle(ctrl, enabled)) {
        ui_custom::draw_button(ctrl, {0, 0, ROW_BTN_W, ROW_BTN_H},
                               enabled ? 0xFF655444 : 0xFF241C18, UI_GOLD, 2,
                               "切换", enabled ? 0xFFFFFFFF : UI_GOLD, 8, 6);
    }
}

void row_desc_draw(void* ctrl) {
    if (g_base == 0) return;
    uint8_t* data = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(ctrl) + CO_DATA);
    if (data == nullptr || data[0] == 0) return;
    ui_draw_text(ctrl, 0, 0x8, UI_GOLD);
}

void address_draw(void* ctrl) {
    ui_draw_text(ctrl, 0, 0x18, UI_GOLD);
}

void back_btn_draw(void* ctrl) {
    if (ctrl == nullptr) return;
    if (g_option_images_loaded && ui_original::draw_back_button(ctrl, g_back_pressed)) return;
    ui_custom::draw_button(ctrl, {0, 0, BACK_BTN_W, BACK_BTN_H}, 0xFF241C18,
                           UI_GOLD, 2, "返回", UI_OPTION_TEXT, 8, 0x18);
}

// ---- 面板控件创建与回调 ----

void settings_panel_enter() {
    SETTINGS_LOG("settings panel enter");
    if (g_root != nullptr) {
        char json[512] = {0};
        if (jni_get_config_json(json, sizeof(json))) {
            parse_config_into_rows(json);
        }
        if (!g_option_images_loaded) {
            g_option_images_loaded = ui_original::load_option_images();
        }
        if (!g_title_background_images_loaded) {
            g_title_background_images_loaded = ui_load_image_unit(TITLE_BACKGROUND_LEFT_UNIT) &&
                ui_load_image_unit(TITLE_BACKGROUND_RIGHT_UNIT);
        }
        g_panel_active = true;
        g_close_delay_frames = 0;
        g_back_pressed = false;
        return;
    }
    if (g_base == 0 || fn_calc_res_width == nullptr || fn_calc_res_height == nullptr) {
        SETTINGS_LOG("settings panel enter: symbols not resolved");
        return;
    }
    // 面板打开时从 Kotlin 拉取当前配置
    char json[512] = {0};
    if (jni_get_config_json(json, sizeof(json))) {
        parse_config_into_rows(json);
    }
    g_option_images_loaded = ui_original::load_option_images();
    g_title_background_images_loaded = ui_load_image_unit(TITLE_BACKGROUND_LEFT_UNIT) &&
        ui_load_image_unit(TITLE_BACKGROUND_RIGHT_UNIT);
    void* root = ui_create_root(settings_root_rect());
    if (root == nullptr) {
        SETTINGS_LOG("settings panel enter: root create failed");
        return;
    }
    UiRect root_rect{};
    ui_get_rect(root, &root_rect);
    g_back_btn = ui_create_button(root, {8 - root_rect.x, 8 - root_rect.y, BACK_BTN_W, BACK_BTN_H}, "返回",
                                  &settings_back_clicked, &back_btn_draw);
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
        UiRect label = settings_label_rect(i);
        void* btn = ui_create_button(root, settings_toggle_rect(i), "切换",
                                     &settings_row_clicked, &row_btn_draw);
        void* desc = ui_create_label(root, label, kRowLabels[i], &row_desc_draw);
        if (btn == nullptr || desc == nullptr) continue;
        g_rows[i].btn = btn;
        g_rows[i].desc = desc;
    }
    void* addr = ui_create_label(root,
                                   {ROOT_W / 2 - 0x38, ADDR_Y, 0x120, ADDR_H},
                                   g_addr_text, &address_draw);
    if (addr != nullptr) {
        g_addr_desc = addr;
    }
    g_root = root;
    g_panel_active = true;
    g_close_delay_frames = 0;
    g_back_pressed = false;
    SETTINGS_LOG("settings panel enter: root=%p %d settings cells + addr created", root, SETTINGS_ROW_COUNT);
}

void settings_panel_process() {
    if (!g_panel_active || g_root == nullptr || g_base == 0) return;
    if (g_close_delay_frames > 0) {
        --g_close_delay_frames;
        if (g_close_delay_frames == 0 && fn_ui_set_popup_process_info != nullptr) {
            fn_ui_set_popup_process_info(3, 0);
            return;
        }
    }
    ui_begin_frame();
    UiRect root_rect{};
    if (!ui_get_rect(g_root, &root_rect)) return;
    const bool background_drawn = g_title_background_images_loaded &&
        ui_draw_image_part(TITLE_BACKGROUND_LEFT_UNIT, TITLE_BACKGROUND_LOC,
                           static_cast<int32_t>(root_rect.x + ROOT_W / 2 - 0x12c),
                           static_cast<int32_t>(root_rect.y + ROOT_H / 2), 2, 1) &&
        ui_draw_image_part(TITLE_BACKGROUND_RIGHT_UNIT, TITLE_BACKGROUND_LOC,
                           static_cast<int32_t>(root_rect.x + ROOT_W / 2 + 0x12c),
                           static_cast<int32_t>(root_rect.y + ROOT_H / 2), 2, 1);
    if (!background_drawn && !g_background_unavailable_logged) {
        SETTINGS_LOG("settings background image unavailable; using restored LCD");
        g_background_unavailable_logged = true;
    }
    UiRect screen_rect = {0, 0, ROOT_W + root_rect.x * 2, ROOT_H};
    ui_fill_rect_alpha(screen_rect, 0xFF000000, 0x50);
    int64_t separators[VISIBLE_GRID_ROWS - 1] = {};
    for (int i = 1; i < VISIBLE_GRID_ROWS; ++i) {
        separators[i - 1] = root_rect.y + GRID_Y + i * CELL_H;
    }
    UiRect grid_rect = {root_rect.x + CONTENT_X, root_rect.y + GRID_Y, CONTENT_W, GRID_H};
    ui_draw_panel_decor(grid_rect, separators, VISIBLE_GRID_ROWS - 1, UI_GOLD);
    for (int i = 0; i <= SETTINGS_COLUMN_COUNT; ++i) {
        ui_draw_vertical_line(root_rect.x + CONTENT_X + i * CELL_W, root_rect.y + GRID_Y,
                              GRID_H, UI_GOLD, 3);
    }
    if (g_back_btn != nullptr && fn_ctrl_btn_draw != nullptr) fn_ctrl_btn_draw(g_back_btn);
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
        if (g_rows[i].btn != nullptr && fn_ctrl_btn_draw != nullptr) fn_ctrl_btn_draw(g_rows[i].btn);
        if (g_rows[i].desc != nullptr && fn_ctrl_btn_draw != nullptr) fn_ctrl_btn_draw(g_rows[i].desc);
    }
    if (g_addr_desc != nullptr && fn_ctrl_btn_draw != nullptr) fn_ctrl_btn_draw(g_addr_desc);
    ui_end_frame();
}

void settings_panel_f3() {
    SETTINGS_LOG("settings panel f3 (terminate)");
    g_panel_active = false;
    g_close_delay_frames = 0;
    g_back_pressed = false;
    if (g_option_images_loaded) {
        ui_original::unload_option_images();
        g_option_images_loaded = false;
    }
}

void settings_panel_f4() {}

void settings_row_clicked(void* ctrl) {
    for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
        if (ctrl != g_rows[i].btn) continue;
        if (!jni_toggle_config(kRowKeys[i])) return;
        char json[512] = {0};
        if (jni_get_config_json(json, sizeof(json))) {
            parse_config_into_rows(json);
            return;
        }
        const bool target_enabled = strcmp(g_row_status[i], "关") == 0;
        snprintf(g_row_status[i], CB_TEXT_SIZE, "%s", target_enabled ? "开" : "关");
        SETTINGS_LOG("settings row%d toggled: %s", i, g_row_status[i]);
        return;
    }
}

void settings_back_clicked(void* ctrl) {
    SETTINGS_LOG("settings back pressed");
    g_back_pressed = true;
}

uint32_t settings_panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (g_root == nullptr || param == 0) return 1;
    if (g_close_delay_frames > 0) return 1;
    if (event == 0x17) {
        int64_t tx = *reinterpret_cast<const int64_t*>(param);
        int64_t ty = *reinterpret_cast<const int64_t*>(param + 8);
        for (int i = 0; i < SETTINGS_ROW_COUNT; ++i) {
            UiRect toggle = settings_toggle_rect(i);
            if (g_rows[i].btn != nullptr &&
                ui_hit_test(g_rows[i].btn, tx, ty, {0, 0, toggle.w, toggle.h})) {
                SETTINGS_LOG("settings event: row%d toggle hit tx=%lld ty=%lld rect=(%lld,%lld)",
                               i, static_cast<long long>(tx), static_cast<long long>(ty),
                               static_cast<long long>(toggle.x), static_cast<long long>(toggle.y));
                settings_row_clicked(g_rows[i].btn);
                break;
            }
        }
        if (g_back_btn != nullptr &&
            ui_hit_test(g_back_btn, tx, ty, {0, 0, BACK_BTN_W, BACK_BTN_H})) {
            settings_back_clicked(g_back_btn);
            return 1;
        }
    } else if (event == 0x18 && g_back_pressed) {
        int64_t tx = *reinterpret_cast<const int64_t*>(param);
        int64_t ty = *reinterpret_cast<const int64_t*>(param + 8);
        const bool released_on_back = g_back_btn != nullptr &&
            ui_hit_test(g_back_btn, tx, ty, {0, 0, BACK_BTN_W, BACK_BTN_H});
        g_back_pressed = false;
        if (released_on_back) {
            SETTINGS_LOG("settings back released -> schedule close");
            g_close_delay_frames = 2;
        }
    }
    return 1;
}

// ---- 主菜单「更多游戏」按钮钩子 ----

void* more_games_btn() {
    if (g_base == 0) return nullptr;
    return *reinterpret_cast<void**>(g_base + G_MAINMENU_BASE_VMA + G_MAINMENU_MOREGAMES_SLOT);
}

void more_games_clicked(void* ctrl) {
    SETTINGS_LOG("more games clicked -> open settings panel");
    if (fn_ui_set_popup_process_info == nullptr || g_state_id < 0) return;
    fn_ui_set_popup_process_info(1, g_state_id);
}

bool inject_more_games_btn() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_more_games_injected) return true;
    if (g_base == 0) return false;
    void* btn = more_games_btn();
    if (btn == nullptr) return false;
    uint8_t* data = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(btn) + CO_DATA);
    if (data == nullptr) return false;
    void** slot = reinterpret_cast<void**>(data + CB_EXECUTE_PROC);
    if (*slot == reinterpret_cast<void*>(&more_games_clicked)) {
        g_more_games_injected = true;
        return true;
    }
    if (!g_more_games_hook.install_typed(slot, &more_games_clicked)) return false;
    g_more_games_injected = true;
    SETTINGS_LOG("more games ExecuteProc hooked: ctrl=%p orig=%p", btn, g_more_games_hook.orig);
    return true;
}

void ensure_inject_thread() {
    if (g_thread_started.exchange(true)) return;
    std::thread([]() {
        for (;;) {
            const char* screen = data_ui_screen();
            bool on_main_menu = screen != nullptr && strcmp(screen, "main_menu") == 0;
            if (on_main_menu) {
                inject_state_entry_locked();
                inject_more_games_btn();
            } else if (g_more_games_injected) {
                // 离开主菜单后控件树被释放，仅重置标志，不 uninstall（避免写已释放内存）
                std::lock_guard<std::mutex> lock(g_settings_mtx);
                g_more_games_injected = false;
                g_more_games_hook.slot = nullptr;
                g_more_games_hook.orig = nullptr;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }).detach();
}

// ---- state list 条目定位与注入 ----

uint8_t* find_state_entry(uintptr_t enter_vma) {
    if (g_base == 0) return nullptr;
    void** got = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (got == nullptr || *got == nullptr) return nullptr;
    uint8_t* list = reinterpret_cast<uint8_t*>(*got);
    for (int i = 0; i < 27; ++i) {
        uintptr_t enter = *reinterpret_cast<uintptr_t*>(list + i * POPUP_STATE_SIZE + 0x10);
        if (enter == g_base + enter_vma) return list + i * POPUP_STATE_SIZE;
    }
    return nullptr;
}

bool inject_state_entry_locked() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_state_entry != nullptr) return true;
    if (g_base == 0) return false;
    uint8_t* entry = find_state_entry(F_PANEL_UNK1_ENTER);
    if (entry == nullptr) {
        SETTINGS_LOG("state entry (INAP_GEMSHOP) not found");
        return false;
    }
    memcpy(g_state_backup, entry, POPUP_STATE_SIZE);
    *reinterpret_cast<uintptr_t*>(entry + 0x10) = reinterpret_cast<uintptr_t>(&settings_panel_enter);
    *reinterpret_cast<uintptr_t*>(entry + 0x18) = reinterpret_cast<uintptr_t>(&settings_panel_process);
    *reinterpret_cast<uintptr_t*>(entry + 0x28) = reinterpret_cast<uintptr_t>(&settings_panel_f3);
    *reinterpret_cast<uintptr_t*>(entry + 0x30) = reinterpret_cast<uintptr_t>(&settings_panel_f4);
    *reinterpret_cast<uintptr_t*>(entry + 0x38) = reinterpret_cast<uintptr_t>(&settings_panel_event);
    g_state_entry = entry;
    g_state_id = *reinterpret_cast<int32_t*>(entry);
    SETTINGS_LOG("state entry %d injected (enter/process/f3/f4/event)", g_state_id);
    return true;
}

}  // namespace

void settings_ui_start_auto_inject() {
    ensure_inject_thread();
}

void settings_register_config_bridge(JNIEnv* env, jclass bridge_class) {
    if (env == nullptr || bridge_class == nullptr) return;
    if (g_config_bridge_class != nullptr) {
        env->DeleteGlobalRef(g_config_bridge_class);
    }
    g_config_bridge_class = static_cast<jclass>(env->NewGlobalRef(bridge_class));
}

std::string data_settings_ui_inject() {
    if (!inject_state_entry_locked()) return op_err("state entry inject failed");
    ensure_inject_thread();
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
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{"
        "\"more_games_injected\":%s,\"more_games_btn\":\"%p\","
        "\"more_games_rect\":[%lld,%lld,%lld,%lld],"
        "\"state_id\":%d,\"state_injected\":%s,"
        "\"panel_active\":%s,\"root\":\"%p\","
        "\"stack_limit_enabled\":%s,"
        "\"screen\":\"%s\"}",
        g_more_games_injected ? "true" : "false", btn,
        static_cast<long long>(bx), static_cast<long long>(by),
        static_cast<long long>(bw), static_cast<long long>(bh),
        g_state_id, g_state_entry != nullptr ? "true" : "false",
        g_panel_active ? "true" : "false", g_root,
        stack_limit_enabled() ? "true" : "false",
        data_ui_screen());
    return std::string(buf);
}

std::string data_settings_ui_restore() {
    std::lock_guard<std::mutex> lock(g_settings_mtx);
    if (g_state_entry != nullptr) {
        memcpy(g_state_entry, g_state_backup, POPUP_STATE_SIZE);
    g_state_entry = nullptr;
        g_state_id = -1;
    }
    g_more_games_hook.uninstall();
    g_more_games_injected = false;
    g_panel_active = false;
    if (g_option_images_loaded) {
        ui_original::unload_option_images();
        g_option_images_loaded = false;
    }
    return op_ok();
}

std::string data_settings_ui_open_panel() {
    if (g_base == 0) return op_err("libgame not ready");
    if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");
    if (g_state_id < 0) return op_err("state not injected");
    fn_ui_set_popup_process_info(1, g_state_id);
    SETTINGS_LOG("open settings panel state id=%d", g_state_id);
    return op_ok();
}

std::string data_settings_ui_open_option() {
    if (g_base == 0) return op_err("libgame not ready");
    if (fn_ui_set_popup_process_info == nullptr) return op_err("symbol not resolved");
    uint8_t* entry = find_state_entry(F_PANEL_OPTIONS_ENTER);
    if (entry == nullptr) return op_err("option state not found");
    int id = *reinterpret_cast<int32_t*>(entry);
    fn_ui_set_popup_process_info(1, id);
    SETTINGS_LOG("open option state id=%d", id);
    return op_ok();
}
