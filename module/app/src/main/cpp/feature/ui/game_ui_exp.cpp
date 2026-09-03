#include "game_ui_exp.h"

#include "game_access.h"
#include "game_ops_common.h"
#include "game_ptr_hook.h"
#include "game_state.h"
#include "game_symbols.h"
#include "game_ui.h"

#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#define EXP_TAG "Inotia4UIExp"
#define EXP_LOG(...) __android_log_print(ANDROID_LOG_INFO, EXP_TAG, __VA_ARGS__)

#define CB_TEXT_SIZE 0x20
#define POPUP_STATE_SIZE 0x40

namespace {

std::mutex g_exp_mtx;

PtrHook g_exp1_hook;
void* g_exp1_btn = nullptr;
bool g_exp1_done = false;

void* g_exp2_btn = nullptr;
void* g_exp2_orig = nullptr;
bool g_exp2_done = false;

char g_exp4_orig_text[CB_TEXT_SIZE] = {0};
bool g_exp4_done = false;

uint8_t g_exp5_backup[POPUP_STATE_SIZE] = {0};
uint8_t* g_exp5_entry = nullptr;
bool g_exp5_done = false;

void exp1_btn_clicked(void* ctrl) {
    EXP_LOG("exp1: settings btn2 ExecuteProc invoked (overridden), ctrl=%p", ctrl);
    if (fn_ctrl_btn_set_text != nullptr) {
        static char t[CB_TEXT_SIZE] = "EXP1-CLICKED";
        fn_ctrl_btn_set_text(ctrl, t);
    }
}

void exp2_btn_clicked(void* ctrl) {
    EXP_LOG("exp2: injected button clicked, ctrl=%p", ctrl);
    if (fn_ctrl_btn_set_text != nullptr) {
        static char t[CB_TEXT_SIZE] = "EXP2-CLICKED";
        fn_ctrl_btn_set_text(ctrl, t);
    }
}

void exp5_enter() {
    EXP_LOG("exp5: custom popup state enter called (injected state entry)");
    if (fn_popup_create != nullptr && g_base != 0) {
        const char* msg = "EXP5: custom state entered";
        fn_popup_create(const_cast<char*>(msg), static_cast<uint32_t>(strlen(msg)), 0, 2);
    }
}

void exp5_noop() {}

void* settings_root() {
    if (g_base == 0) return nullptr;
    return *reinterpret_cast<void**>(g_base + G_UISETTINGS_VMA + 0x100);
}

void* settings_btn2() {
    if (g_base == 0) return nullptr;
    return *reinterpret_cast<void**>(g_base + G_UISETTINGS_VMA + 0x120);
}

uint8_t* btn_data(void* btn) {
    if (btn == nullptr) return nullptr;
    return *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(btn) + CO_DATA);
}

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

}  // namespace

std::string data_exp1_btn_behavior() {
    std::lock_guard<std::mutex> lock(g_exp_mtx);
    if (g_exp1_done) return op_err("exp1 already injected");
    if (!game_in_world()) return op_err("not in game");
    if (fn_ctrl_btn_set_text == nullptr) return op_err("symbol not resolved");
    void* btn = settings_btn2();
    if (btn == nullptr) return op_err("settings panel not open");
    uint8_t* data = btn_data(btn);
    if (data == nullptr) return op_err("button data missing");
    void** slot = reinterpret_cast<void**>(data + CB_EXECUTE_PROC);
    if (*slot == nullptr) return op_err("execute proc empty");
    if (!g_exp1_hook.install_typed(slot, &exp1_btn_clicked)) {
        return op_err("hook install failed");
    }
    g_exp1_btn = btn;
    g_exp1_done = true;
    EXP_LOG("exp1: settings btn2 ExecuteProc hooked, ctrl=%p orig=%p", btn, g_exp1_hook.orig);
    exp1_btn_clicked(btn);
    return op_ok();
}

std::string data_exp2_add_control() {
    std::lock_guard<std::mutex> lock(g_exp_mtx);
    if (g_exp2_done) return op_err("exp2 already injected");
    if (!game_in_world()) return op_err("not in game");
    if (fn_ctrl_btn_create == nullptr || fn_ctrl_set_event_call_type == nullptr) {
        return op_err("symbol not resolved");
    }
    void* root = settings_root();
    if (root == nullptr) return op_err("settings panel not open");
    // 绘制机制：Scene_Draw_POPUP_SC_SYSTEMMENU 硬编码枚举固定槽（按钮1@+0x108、菜单组 6 子控件、按钮2/3/4@+0x120/128/130），
    // 挂根控件的新按钮不会被绘制 → 注入按钮必须写回枚举槽（与 craft 按钮注入同模式）。
    void* old_btn = settings_btn2();
    if (old_btn == nullptr) return op_err("btn2 slot empty");
    static char text[CB_TEXT_SIZE] = "EXP2-BTN";
    void* btn = fn_ctrl_btn_create(root, text);
    if (btn == nullptr) return op_err("button create failed");
    fn_ctrl_set_event_call_type(btn, 0x200);
    uint8_t* new_data = btn_data(btn);
    uint8_t* old_data = btn_data(old_btn);
    if (new_data != nullptr && old_data != nullptr) {
        // 完整复制原按钮绘制属性（DrawType/DrawID/DrawSubID/DrawProc/State/Enabled/Text）
        memcpy(new_data, old_data, CB_SIZE);
        // 覆盖 ExecuteProc 为自定义点击回调（行为改写验证）
        *reinterpret_cast<void**>(new_data + CB_EXECUTE_PROC) = reinterpret_cast<void*>(&exp2_btn_clicked);
        static char t2[CB_TEXT_SIZE] = "EXP2-BTN";
        fn_ctrl_btn_set_text(btn, t2);
    }
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(btn) + CO_ACTIVE) = 0x20;
    // 写回绘制枚举槽
    *reinterpret_cast<void**>(g_base + G_UISETTINGS_VMA + 0x120) = btn;
    g_exp2_btn = btn;
    g_exp2_orig = old_btn;
    g_exp2_done = true;
    EXP_LOG("exp2: injected button ctrl=%p replaced slot btn2=%p", btn, old_btn);
    return op_ok();
}

std::string data_exp3_custom_dialog(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_exp_mtx);
    if (g_base == 0) return op_err("libgame not ready");
    if (fn_popup_create == nullptr) return op_err("symbol not resolved");
    if (fn_popup_free != nullptr) fn_popup_free();
    // UIPopupMsg_Create w1 bit0=0 为引用语义（不拷贝）→ 必须用持久缓冲，引擎持有指针直到弹窗关闭
    static char buf[512];
    snprintf(buf, sizeof(buf), "%s", text.c_str());
    fn_popup_create(buf, 2, 0, 2);
    std::string out = "{\"ok\":true";
    if (g_popup_on != nullptr) {
        out += ",\"popup_on\":" + std::to_string(static_cast<int>(*reinterpret_cast<uint8_t*>(g_popup_on)));
    }
    out += "}";
    return out;
}

std::string data_exp4_text_appearance() {
    std::lock_guard<std::mutex> lock(g_exp_mtx);
    if (g_exp4_done) return op_err("exp4 already applied");
    if (!game_in_world()) return op_err("not in game");
    if (fn_ctrl_btn_set_text == nullptr || fn_ctrl_btn_set_draw_id == nullptr) {
        return op_err("symbol not resolved");
    }
    void* btn = settings_btn2();
    if (btn == nullptr) return op_err("settings panel not open");
    uint8_t* data = btn_data(btn);
    if (data == nullptr) return op_err("button data missing");
    memcpy(g_exp4_orig_text, data, CB_TEXT_SIZE);
    static char new_text[CB_TEXT_SIZE] = "EXP4-MODIFIED";
    fn_ctrl_btn_set_text(btn, new_text);
    fn_ctrl_btn_set_draw_id(btn, 2);
    g_exp4_done = true;
    EXP_LOG("exp4: settings btn2 text/draw modified");
    return op_ok();
}

std::string data_exp5_new_panel() {
    std::lock_guard<std::mutex> lock(g_exp_mtx);
    if (g_exp5_done) return op_err("exp5 already injected");
    if (g_base == 0) return op_err("libgame not ready");
    if (fn_popupstate_push == nullptr || fn_ui_set_popup_process_info == nullptr) {
        return op_err("symbol not resolved");
    }
    uint8_t* entry = find_state_entry(F_PANEL_DAILY_REWARD_ENTER);
    if (entry == nullptr) return op_err("state entry not found");
    memcpy(g_exp5_backup, entry, POPUP_STATE_SIZE);
    *reinterpret_cast<uintptr_t*>(entry + 0x10) = reinterpret_cast<uintptr_t>(&exp5_enter);
    *reinterpret_cast<uintptr_t*>(entry + 0x18) = reinterpret_cast<uintptr_t>(&exp5_noop);
    *reinterpret_cast<uintptr_t*>(entry + 0x28) = reinterpret_cast<uintptr_t>(&exp5_noop);
    *reinterpret_cast<uintptr_t*>(entry + 0x30) = reinterpret_cast<uintptr_t>(&exp5_noop);
    *reinterpret_cast<uintptr_t*>(entry + 0x38) = reinterpret_cast<uintptr_t>(&exp5_noop);
    g_exp5_entry = entry;
    g_exp5_done = true;
    int state_id = *reinterpret_cast<int32_t*>(entry);
    EXP_LOG("exp5: state entry %d injected, enter->exp5_enter", state_id);
    fn_ui_set_popup_process_info(1, state_id);
    std::string out = "{\"ok\":true,\"state_id\":" + std::to_string(state_id) + "}";
    return out;
}

std::string data_exp_restore_all() {
    std::lock_guard<std::mutex> lock(g_exp_mtx);
    if (g_exp1_done && g_exp1_btn != nullptr) {
        g_exp1_hook.uninstall();
        g_exp1_btn = nullptr;
        g_exp1_done = false;
    }
    if (g_exp2_done && g_exp2_orig != nullptr) {
        *reinterpret_cast<void**>(g_base + G_UISETTINGS_VMA + 0x120) = g_exp2_orig;
        g_exp2_btn = nullptr;
        g_exp2_orig = nullptr;
        g_exp2_done = false;
    }
    if (g_exp4_done) {
        void* btn = settings_btn2();
        if (btn != nullptr && fn_ctrl_btn_set_text != nullptr) {
            fn_ctrl_btn_set_text(btn, g_exp4_orig_text);
        }
        g_exp4_done = false;
    }
    if (g_exp5_done && g_exp5_entry != nullptr) {
        memcpy(g_exp5_entry, g_exp5_backup, POPUP_STATE_SIZE);
        g_exp5_entry = nullptr;
        g_exp5_done = false;
    }
    if (g_popup_on != nullptr && *reinterpret_cast<uint8_t*>(g_popup_on) && fn_popup_free != nullptr) {
        fn_popup_free();
        *reinterpret_cast<uint8_t*>(g_popup_on) = 0;  // popup_free 不清 bOn（ButtonOKExe 才清）
    }
    return op_ok();
}

std::string data_exp_status_json() {
    std::lock_guard<std::mutex> lock(g_exp_mtx);
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{"
        "\"exp1_hooked\":%s,\"exp1_btn\":\"%p\","
        "\"exp2_injected\":%s,\"exp2_btn\":\"%p\","
        "\"exp4_applied\":%s,"
        "\"exp5_injected\":%s,\"exp5_state_id\":%d,"
        "\"settings_root\":\"%p\",\"settings_btn2\":\"%p\","
        "\"popup_on\":%u",
        g_exp1_done ? "true" : "false", g_exp1_btn,
        g_exp2_done ? "true" : "false", g_exp2_btn,
        g_exp4_done ? "true" : "false",
        g_exp5_done ? "true" : "false",
        g_exp5_entry != nullptr ? *reinterpret_cast<int32_t*>(g_exp5_entry) : -1,
        settings_root(), settings_btn2(),
        g_popup_on != nullptr ? static_cast<unsigned>(*reinterpret_cast<uint8_t*>(g_popup_on)) : 0u);
    if (g_base != 0) {
        uint8_t* pt = *reinterpret_cast<uint8_t**>(g_base + G_POPUP_TEXT_VMA);
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ",\"popup_text_hex\":\"");
        if (pt != nullptr) {
            for (int i = 0; i < 24 && pt[i] != 0; ++i) {
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%02x", pt[i]);
            }
        }
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "\"");
        void* b2 = settings_btn2();
        uint8_t* b2d = b2 != nullptr ? btn_data(b2) : nullptr;
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ",\"btn2_text_hex\":\"");
        if (b2d != nullptr) {
            for (int i = 0; i < 16 && b2d[i] != 0; ++i) {
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%02x", b2d[i]);
            }
        }
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "\"");
    }
    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ",\"screen\":\"%s\"}", data_ui_screen());
    return std::string(buf);
}
