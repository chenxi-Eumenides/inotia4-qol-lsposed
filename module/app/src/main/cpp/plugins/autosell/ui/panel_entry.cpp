#include "panel_internal.h"

namespace qol::features::autosell {

// 开关：JVM 线程写、游戏主线程读；默认关闭。
std::atomic<bool> g_enabled{false};
// 入口控件引用：主线程与 JVM 线程都可能写，用互斥保护。
std::mutex g_entry_mtx;
// 入口按钮的锚定状态：句柄存活/身份校验/失效重建由 core.custom_ui 管理。
qol::contracts::QolUiAnchoredButton g_entry{};

// ---- 入口按钮 ----
// 入口是挂在原版袋容器（游戏控件树）上的图标按钮：native 生命周期归游戏，面板切页/
// 关闭时整棵子树被销毁。句柄存活、身份校验与失效重建统一由 core.custom_ui 的
// 「锚定按钮」负责（ui_components minor 20）；本文件只提供几何与点击行为。

static void autosell_entry_clicked(void* ctrl) {
    (void)ctrl;
    const auto* panels = autosell_panels_api();
    if (panels == nullptr || panels->open == nullptr || !panels->open("autosell")) {
        AUTOSELLUI_LOG("entry click rejected: ui_panels open failed");
    }
}

// 布局回调：parent 是当帧活的袋容器。取原版袋 0 绝对位置 + 偏移、
// 任务袋（child 5）行对齐，换算成相对 parent 的 rect。
static QolResult autosell_entry_layout(void* parent, UiRect* out_rect, void* user) {
    (void)user;
    if (parent == nullptr || out_rect == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "entry layout", 12);
    }
    constexpr uint32_t kRequiredChildren = 6;
    uint32_t child_count = 0;
    if (!ui_child_count(parent, &child_count) || child_count < kRequiredChildren) {
        // 占位容器（原版 6 袋标签未就位）：本帧无法定位，下一帧重试。
        return qol::contracts::error(kQolErrNotReady, "bag container not ready", 24);
    }
    void* bag0 = nullptr;
    UiRect bag0_rect{};
    if (!ui_child_at(parent, 0, &bag0) || bag0 == nullptr || !ui_get_rect(bag0, &bag0_rect) ||
        bag0_rect.w <= 0) {
        return qol::contracts::error(kQolErrNotReady, "bag0 not ready", 15);
    }
    int64_t bag0_abs_x = 0, bag0_abs_y = 0;
    int64_t parent_abs_x = 0, parent_abs_y = 0;
    autosell_ctrl_abs(bag0, &bag0_abs_x, &bag0_abs_y);
    autosell_ctrl_abs(parent, &parent_abs_x, &parent_abs_y);
    // 横向中心 = 扩展页签列中心；纵向中心 = 任务背包格中心。
    const int64_t center_x = bag0_abs_x + kExtTabOffsetX + kBagTabSize / 2;
    int64_t center_y = bag0_abs_y + 347 + kBagTabSize / 2;  // 任务袋缺失时的兜底
    void* task_bag = nullptr;
    UiRect task{};
    if (ui_child_at(parent, 5, &task_bag) && task_bag != nullptr &&
        ui_get_rect(task_bag, &task) && task.h > 0) {
        int64_t task_abs_x = 0, task_abs_y = 0;
        autosell_ctrl_abs(task_bag, &task_abs_x, &task_abs_y);
        (void)task_abs_x;
        center_y = task_abs_y + task.h / 2;
    }
    *out_rect = {center_x - kEntrySize / 2 - parent_abs_x,
        center_y + kEntryOffsetY - kEntrySize / 2 - parent_abs_y, kEntrySize, kEntrySize};
    static UiRect last_rect{};
    if (last_rect.x != out_rect->x || last_rect.y != out_rect->y ||
        last_rect.w != out_rect->w || last_rect.h != out_rect->h) {
        last_rect = *out_rect;
        AUTOSELLUI_LOG("entry rect=(%lld,%lld,%lld,%lld) center=(%lld,%lld) bag0=(%lld,%lld) "
                       "task_center_y=%lld",
                       static_cast<long long>(out_rect->x), static_cast<long long>(out_rect->y),
                       static_cast<long long>(out_rect->w), static_cast<long long>(out_rect->h),
                       static_cast<long long>(center_x), static_cast<long long>(center_y),
                       static_cast<long long>(bag0_abs_x), static_cast<long long>(bag0_abs_y),
                       static_cast<long long>(center_y));
    }
    return qol::contracts::ok();
}

// 每帧（EQUIP 页）确保入口可见并绘制；页面不在/控件被游戏销毁时由锚定层自动重建。
static void autosell_on_equip_draw() {
    std::lock_guard<std::mutex> lock(g_entry_mtx);
    const auto* components = autosell_components();
    if (components == nullptr) return;
    if (!g_enabled.load(std::memory_order_acquire)) {
        if (components->anchored_button_set_visible != nullptr)
            (void)components->anchored_button_set_visible(&g_entry, 0);
        return;
    }
    if (components->anchored_button_ensure == nullptr ||
        components->anchored_button_draw == nullptr) {
        return;
    }
    const qol::contracts::UiIconButtonDesc button{{0, 0, kEntrySize, kEntrySize},
        kGearImageUnit, kGearImageLoc, 1, 1, "售", &autosell_entry_clicked, nullptr, 0};
    const qol::contracts::UiAnchoredButtonDesc desc{
        static_cast<uint32_t>(qol::contracts::QolUiRootKind::kEquipBagContainer), button};
    uint8_t created = 0;
    if (components->anchored_button_ensure(&g_entry, &desc, &autosell_entry_layout, nullptr,
            &created).ok != kQolOk) {
        return;
    }
    if (created != 0) {
        AUTOSELLUI_LOG("entry installed via anchored button");
    }
    (void)components->anchored_button_draw(&g_entry);
}

// 复刻原 `bl UIDesc_Draw`，随后绘制入口按钮（独立于扩展背包绘制宿主）。
// kCallPatch 站点 `patch:SCENE_DRAW_EQUIP+0x1cc` 的 handler（宿主安装时写入 BL）。
void autosell_equip_desc_draw_wrapper() {
    ui_uidesc_draw();
    autosell_on_equip_draw();
}

bool autosell_ui_enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void autosell_ui_set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
    if (enabled) {
        // 开启：不在此线程触碰游戏控件树；下一次 EQUIP 页绘制时安装按钮。
        return;
    }
    // 不从非游戏线程触碰入口控件；下次装备页绘制时禁用原有组件，不重建。
    // 面板正打开则请求关闭（弹栈后游戏会回调 f3 清理面板状态）。
    if (g_panel_active.load(std::memory_order_acquire)) {
        autosell_request_panel_close();
        AUTOSELLUI_LOG("disabled -> panel close requested");
    }
}

}  // namespace qol::features::autosell
