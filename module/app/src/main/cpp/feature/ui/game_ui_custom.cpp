#include "game_ui_custom.h"

#include "core/native/qol_log.h"
#include "game_access.h"
#include "game_ops_common.h"
#include "feature/patch/game_patch.h"
#include "game_ptr_hook.h"
#include "game_state.h"
#include "game_symbols.h"
#include "game_ui.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#define CUSTOM_LOG(...) QOL_LOG_INFO(QolDomain::kUi, __VA_ARGS__)

#define POPUP_STATE_SIZE 0x40
#define CB_TEXT_SIZE 0x20

// 面板布局（逻辑坐标 0-960×0-640 空间，相对全屏居中 root；触摸坐标同空间）
#define ROOT_W 0x3c0
#define ROOT_H 0x280
#define BTN_W 0x120
#define BTN_H 0x50
#define DESC_W 0x120
#define DESC_H 0x30
#define CELL_COL0_X 0x50
#define CELL_COL1_X 0x1b0
#define CELL_ROW0_Y 0x60
#define CELL_ROW1_Y 0x120
#define CELL_ROW2_Y 0x1e0
#define BTN_IMG_OFF_X (-0xc)
#define BTN_IMG_OFF_Y (-0x9)

namespace {

std::mutex g_custom_mtx;
PtrHook g_store_back_hook;
bool g_btn_injected = false;
std::atomic<bool> g_btn_want{false};
std::atomic<bool> g_thread_started{false};

int font_id();

bool g_panel_active = false;
uint8_t* g_state_entry = nullptr;
uint8_t g_state_backup[POPUP_STATE_SIZE] = {0};
int g_state_id = -1;
void* g_root = nullptr;

struct CustomCell {
    void* btn;
    void* desc;
};
CustomCell g_cells[3];
static void (*g_cell_procs[3])(void*) = {nullptr, nullptr, nullptr};

// ControlObject_SetRect 有 AArch64 x8 输出参数（GetRelativeRect 写 x8）→ C++ 无法传 x8，
// 直接写 [ctrl+0x18..0x30] 内存（docs/system/ui.md §6 关键坑 3）。
void set_ctrl_rect(void* ctrl, int64_t x, int64_t y, int64_t w, int64_t h) {
    uint8_t* c = reinterpret_cast<uint8_t*>(ctrl);
    *reinterpret_cast<int64_t*>(c + CO_RECT_X) = x;
    *reinterpret_cast<int64_t*>(c + CO_RECT_Y) = y;
    *reinterpret_cast<int64_t*>(c + CO_RECT_W) = w;
    *reinterpret_cast<int64_t*>(c + CO_RECT_H) = h;
}

// ControlObject_GetAbsoluteRect 同样用 x8 输出（GetRelativeRect stp [x8]）→ C++ 无法传 x8
// （真机 SIGSEGV 实测）。自实现：累加父链 rect（控件树固定两层 root+子，root 无父）。
// 输出 [abs_x, abs_y]（w/h 用贴图固定尺寸，GetAbsoluteRect 也只输出 x/y 两个 i64）。
void ctrl_abs_point(void* ctrl, int64_t out[2]) {
    uint8_t* c = reinterpret_cast<uint8_t*>(ctrl);
    int64_t x = *reinterpret_cast<int64_t*>(c + CO_RECT_X);
    int64_t y = *reinterpret_cast<int64_t*>(c + CO_RECT_Y);
    void* p = *reinterpret_cast<void**>(c + CO_PARENT);
    while (p != nullptr) {
        uint8_t* pc = reinterpret_cast<uint8_t*>(p);
        x += *reinterpret_cast<int64_t*>(pc + CO_RECT_X);
        y += *reinterpret_cast<int64_t*>(pc + CO_RECT_Y);
        p = *reinterpret_cast<void**>(pc + CO_PARENT);
    }
    out[0] = x;
    out[1] = y;
}

// 官方 UI_DrawStringHAlign 的 font 参数来自 [0x2f3000+0x628] 指向配置的 +4（UIWorldMap_Draw 0xd3684）
int font_id() {
    if (g_base == 0) return 1;
    void** got = reinterpret_cast<void**>(g_base + G_FONT_CONFIG_GOT_VMA);
    if (got == nullptr || *got == nullptr) return 1;
    return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(*got) + 4);
}


#include "game_ui_custom_panel.inc"
#include "game_ui_custom_injection.inc"
