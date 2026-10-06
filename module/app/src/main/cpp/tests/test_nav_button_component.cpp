#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "contracts/ui.h"
#include "plugins/core.custom_ui/ui_components.h"

// 导航按钮点击内部调用 ui_panels_request_back，自适应绘制按 ui_panels_depth 选图；
// 本测试只链接 ui_components.cpp，不链接面板栈实现，故在此补齐这两个外部符号。
// 必须在定义 core_ui 函数前声明计数与深度，供其引用。
namespace {
int g_back_requests = 0;
int g_fake_depth = 1;
}  // namespace

namespace qol::features::core_ui {
void ui_panels_request_back() { ++g_back_requests; }
int ui_panels_depth() { return g_fake_depth; }
}  // namespace qol::features::core_ui

namespace {

struct FakeControl {
    uint32_t kind = 0;
    FakeControl* parent = nullptr;
    qol::contracts::UiRect rect{};
    qol::contracts::QolUiClickFn click = nullptr;
    void* click_user = nullptr;
    uint8_t pressed = 0;
    bool enabled = true;
};

int g_clicks = 0;
int g_fake_main_thread = 1;
int g_close_draws = 0;
int32_t g_close_pressed = -1;
qol::contracts::UiRect g_close_rect{};
int g_back_draws = 0;
int32_t g_back_pressed = -1;
qol::contracts::QolUiHandle g_back_handle = nullptr;
int g_nav_deletes = 0;
int g_option_image_loads = 0;

using qol::contracts::UiRect;
using qol::contracts::QolUiAdapter;
using qol::contracts::QolUiHandle;

QolResult ok() { return qol::contracts::ok(); }

QolResult invalid() { return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7); }

void on_click(void*) { ++g_clicks; }

QolResult create_control(uint32_t kind, qol::contracts::QolUiHandle parent,
                         qol::contracts::QolUiHandle* out) {
    if (out == nullptr) return invalid();
    auto* control = new FakeControl();
    control->kind = kind;
    control->parent = static_cast<FakeControl*>(parent);
    *out = control;
    return ok();
}

QolResult set_rect(qol::contracts::QolUiHandle handle, UiRect rect) {
    if (handle == nullptr) return invalid();
    static_cast<FakeControl*>(handle)->rect = rect;
    return ok();
}

QolResult set_click_proc(qol::contracts::QolUiHandle handle, qol::contracts::QolUiClickFn click,
                         void* user) {
    if (handle == nullptr || click == nullptr) return invalid();
    auto* control = static_cast<FakeControl*>(handle);
    control->click = click;
    control->click_user = user;
    return ok();
}

QolResult set_button_enabled(qol::contracts::QolUiHandle handle, uint8_t enabled) {
    if (handle == nullptr || enabled > 1) return invalid();
    static_cast<FakeControl*>(handle)->enabled = enabled != 0;
    return ok();
}

QolResult draw_native_close_button(UiRect rect, uint8_t pressed) {
    if (pressed > 1) return invalid();
    ++g_close_draws;
    g_close_pressed = pressed;
    g_close_rect = rect;
    return ok();
}

QolResult draw_back_button(qol::contracts::QolUiHandle handle, uint8_t pressed) {
    if (handle == nullptr || pressed > 1) return invalid();
    ++g_back_draws;
    g_back_pressed = pressed;
    g_back_handle = handle;
    return ok();
}

QolResult control_pressed(qol::contracts::QolUiHandle handle, int32_t* out) {
    if (handle == nullptr || out == nullptr) return invalid();
    *out = static_cast<FakeControl*>(handle)->pressed;
    return ok();
}

QolResult control_absolute_origin(qol::contracts::QolUiHandle handle, int64_t* out_x,
                                  int64_t* out_y) {
    if (handle == nullptr || out_x == nullptr || out_y == nullptr) return invalid();
    int64_t x = 0;
    int64_t y = 0;
    for (const FakeControl* current = static_cast<const FakeControl*>(handle); current != nullptr;
         current = current->parent) {
        x += current->rect.x;
        y += current->rect.y;
    }
    *out_x = x;
    *out_y = y;
    return ok();
}

// 模拟引擎事件树语义：按下落在按钮内置 enabled，抬起仍在按钮内且 enabled 才触发点击；
// 移出后抬起取消。与 test_close_button_component.cpp 的同款 fake 保持一致。
QolResult control_tree_event(qol::contracts::QolUiHandle handle, uint64_t event, uint64_t param,
                             uint64_t, uint32_t* consumed) {
    if (handle == nullptr || param == 0 || consumed == nullptr) return invalid();
    const auto* point = reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(param));
    auto* control = static_cast<FakeControl*>(handle);
    int64_t x = 0;
    int64_t y = 0;
    for (const FakeControl* current = control; current != nullptr; current = current->parent) {
        x += current->rect.x;
        y += current->rect.y;
    }
    const bool inside = point[0] >= x && point[0] < x + control->rect.w &&
        point[1] >= y && point[1] < y + control->rect.h;
    if (event == 0x17) {
        control->enabled = inside;
        *consumed = inside ? 1u : 0u;
    } else if (event == 0x18) {
        if (control->enabled && inside && control->click != nullptr) {
            control->click(control->click_user);
        }
        control->enabled = false;
        *consumed = inside ? 1u : 0u;
    }
    return ok();
}

QolResult delete_control(qol::contracts::QolUiHandle handle) {
    if (handle == nullptr) return invalid();
    ++g_nav_deletes;
    return ok();
}

QolResult load_option_images() {
    ++g_option_image_loads;
    return ok();
}

int32_t is_main_thread() { return g_fake_main_thread; }

QolUiAdapter make_adapter() {
    QolUiAdapter adapter{};
    adapter.size = sizeof(QolUiAdapter);
    adapter.major = qol::contracts::kUiAdapterMajor;
    adapter.minor = qol::contracts::kUiAdapterMinor;
    adapter.create_control = &create_control;
    adapter.set_rect = &set_rect;
    adapter.set_click_proc = &set_click_proc;
    adapter.set_button_enabled = &set_button_enabled;
    adapter.draw_native_close_button = &draw_native_close_button;
    adapter.draw_back_button = &draw_back_button;
    adapter.control_pressed = &control_pressed;
    adapter.control_absolute_origin = &control_absolute_origin;
    adapter.control_tree_event = &control_tree_event;
    adapter.delete_control = &delete_control;
    adapter.is_main_thread = &is_main_thread;
    adapter.load_option_images = &load_option_images;
    return adapter;
}

}  // namespace

int main() {
    int assertions = 0;
#define CHECK(condition) \
    do {                    \
        assert(condition);  \
        ++assertions;       \
    } while (false)

    using qol::contracts::QolUiComponents;

    // 表尾纯追加：close 三指针前缀成员保持原偏移，nav 三项位于其后且可用。
    static_assert(offsetof(QolUiComponents, nav_button_create) >
        offsetof(QolUiComponents, close_button_draw), "nav entries must append the table");
    const QolUiComponents* components = qol::features::core_ui::ui_components_api();
    CHECK(components->close_button_create != nullptr &&
          components->close_button_set_enabled != nullptr &&
          components->close_button_draw != nullptr);
    CHECK(components->nav_button_create != nullptr &&
          components->nav_button_set_enabled != nullptr &&
          components->nav_button_draw != nullptr);
    CHECK(components->major == qol::contracts::kUiContractMajor &&
          components->minor == qol::contracts::kUiContractMinor);
    CHECK(components->size == sizeof(QolUiComponents));

    QolUiAdapter adapter = make_adapter();
    CHECK(qol::features::core_ui::ui_components_bind(&adapter));

    QolUiHandle root = nullptr;
    const qol::contracts::UiPanelDesc panel_desc{{0, 0, 480, 320}, {0, 0, 480, 320}, 0, 0};
    CHECK(components->panel_create(&panel_desc, nullptr, &root).ok == kQolOk);

    const qol::contracts::UiNavButtonDesc auto_desc{
        {16, 16, 48, 48}, qol::contracts::kUiNavButtonAuto, &on_click, nullptr};

    // 旧 minor（15）adapter 可通过 bind 基线（13），但 nav_button_create 要求 minor 16。
    {
        QolUiAdapter old_adapter = make_adapter();
        old_adapter.minor = 15;
        CHECK(qol::features::core_ui::ui_components_bind(&old_adapter));
        QolUiHandle stale = nullptr;
        CHECK(components->nav_button_create(&auto_desc, root, &stale).code == kQolErrNotReady);
        CHECK(stale == nullptr);
        CHECK(qol::features::core_ui::ui_components_bind(&adapter));
    }

    // 空参数与非 panel 组件语义：desc/out 为空、非法 mode、扁平 rect、parent 为空均拒绝。
    QolUiHandle nav = nullptr;
    CHECK(components->nav_button_create(nullptr, root, &nav).code == kQolErrInvalidArgument);
    CHECK(nav == nullptr);
    CHECK(components->nav_button_create(&auto_desc, root, nullptr).code ==
          kQolErrInvalidArgument);
    const qol::contracts::UiNavButtonDesc bad_mode{
        {16, 16, 48, 48}, 3, &on_click, nullptr};
    CHECK(components->nav_button_create(&bad_mode, root, &nav).code == kQolErrInvalidArgument);
    CHECK(nav == nullptr);
    const qol::contracts::UiNavButtonDesc flat_rect{
        {16, 16, 0, 48}, qol::contracts::kUiNavButtonAuto, &on_click, nullptr};
    CHECK(components->nav_button_create(&flat_rect, root, &nav).code == kQolErrInvalidArgument);
    CHECK(nav == nullptr);
    // nav 按钮为非 panel 组件：parent 为空直接 invalid_argument。
    CHECK(components->nav_button_create(&auto_desc, nullptr, &nav).code ==
          kQolErrInvalidArgument);
    CHECK(nav == nullptr);
    // on_click 可空：附加通知可选，弹栈行为不依赖它。
    const qol::contracts::UiNavButtonDesc no_click{
        {16, 16, 48, 48}, qol::contracts::kUiNavButtonAuto, nullptr, nullptr};
    QolUiHandle silent = nullptr;
    CHECK(components->nav_button_create(&no_click, root, &silent).ok == kQolOk);
    CHECK(silent != nullptr);
    CHECK(components->destroy(silent).ok == kQolOk);

    CHECK(components->nav_button_create(&auto_desc, root, &nav).ok == kQolOk);
    CHECK(nav != nullptr);

    // auto + 栈深 1：走 X 关闭绘制入口（close 贴图），按控件绝对原点给出 rect。
    g_fake_depth = 1;
    g_close_draws = 0;
    g_back_draws = 0;
    CHECK(components->nav_button_draw(nav).ok == kQolOk);
    CHECK(g_close_draws == 1 && g_back_draws == 0);
    CHECK(g_close_pressed == 0);
    CHECK(g_close_rect.x == 16 && g_close_rect.y == 16 && g_close_rect.w == 48 &&
          g_close_rect.h == 48);
    static_cast<FakeControl*>(nav)->pressed = 1;
    CHECK(components->nav_button_draw(nav).ok == kQolOk);
    CHECK(g_close_draws == 2 && g_close_pressed == 1);
    static_cast<FakeControl*>(nav)->pressed = 0;

    // auto + 栈深 2：走返回箭头绘制入口，直接以控件句柄绘制，不读绝对原点。
    g_fake_depth = 2;
    g_back_draws = 0;
    g_back_pressed = -1;
    g_back_handle = nullptr;
    CHECK(components->nav_button_draw(nav).ok == kQolOk);
    CHECK(g_back_draws == 1 && g_back_pressed == 0 && g_back_handle == nav);
    CHECK(g_close_draws == 2);
    static_cast<FakeControl*>(nav)->pressed = 1;
    CHECK(components->nav_button_draw(nav).ok == kQolOk);
    CHECK(g_back_draws == 2 && g_back_pressed == 1);
    static_cast<FakeControl*>(nav)->pressed = 0;

    // auto + 栈深 0：非 >1，安全回落到 X 绘制入口。
    g_fake_depth = 0;
    const int close_before = g_close_draws;
    CHECK(components->nav_button_draw(nav).ok == kQolOk);
    CHECK(g_close_draws == close_before + 1 && g_back_draws == 2);

    // 强制模式覆盖栈深：ForceClose 恒走 X，ForceBack 恒走箭头。
    const qol::contracts::UiNavButtonDesc force_close_desc{
        {16, 16, 48, 48}, qol::contracts::kUiNavButtonForceClose, &on_click, nullptr};
    const qol::contracts::UiNavButtonDesc force_back_desc{
        {16, 16, 48, 48}, qol::contracts::kUiNavButtonForceBack, &on_click, nullptr};
    QolUiHandle force_close = nullptr;
    QolUiHandle force_back = nullptr;
    CHECK(components->nav_button_create(&force_close_desc, root, &force_close).ok == kQolOk);
    CHECK(components->nav_button_create(&force_back_desc, root, &force_back).ok == kQolOk);
    g_fake_depth = 2;
    g_close_draws = 0;
    g_back_draws = 0;
    CHECK(components->nav_button_draw(force_close).ok == kQolOk);
    CHECK(g_close_draws == 1 && g_back_draws == 0);
    CHECK(components->nav_button_draw(force_back).ok == kQolOk);
    CHECK(g_close_draws == 1 && g_back_draws == 1 && g_back_handle == force_back);
    g_fake_depth = 1;
    CHECK(components->nav_button_draw(force_back).ok == kQolOk);
    CHECK(g_close_draws == 1 && g_back_draws == 2);

    // 点击：经命中按下+释放触发一次 request_back 与附加 on_click；移出后抬起取消；
    // 外部按下不触发。
    g_fake_depth = 1;
    g_back_requests = 0;
    g_clicks = 0;
    int64_t inside_point[2] = {20, 20};
    int64_t outside_point[2] = {400, 400};
    uint32_t consumed = 0;
    CHECK(components->dispatch_native_event(nav, 0x17,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside_point)), 0, &consumed).ok ==
        kQolOk);
    CHECK(components->dispatch_native_event(nav, 0x18,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside_point)), 0, &consumed).ok ==
        kQolOk);
    CHECK(g_back_requests == 1 && g_clicks == 1);
    CHECK(components->dispatch_native_event(nav, 0x17,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside_point)), 0, &consumed).ok ==
        kQolOk);
    CHECK(components->dispatch_native_event(nav, 0x18,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(outside_point)), 0, &consumed).ok ==
        kQolOk);
    CHECK(g_back_requests == 1 && g_clicks == 1);
    CHECK(components->dispatch_native_event(nav, 0x17,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(outside_point)), 0, &consumed).ok ==
        kQolOk);
    CHECK(components->dispatch_native_event(nav, 0x18,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside_point)), 0, &consumed).ok ==
        kQolOk);
    CHECK(g_back_requests == 1 && g_clicks == 1);
    CHECK(components->dispatch_native_event(nav, 0x17,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside_point)), 0, &consumed).ok ==
        kQolOk);
    CHECK(components->dispatch_native_event(nav, 0x18,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside_point)), 0, &consumed).ok ==
        kQolOk);
    CHECK(g_back_requests == 2 && g_clicks == 2);

    // 禁用后点击不响应；重新启用后恢复；非法 enabled 拒绝。
    auto* fake = static_cast<FakeControl*>(nav);
    fake->click(fake->click_user);
    CHECK(g_back_requests == 3 && g_clicks == 3);
    CHECK(components->nav_button_set_enabled(nav, 0).ok == kQolOk);
    fake->click(fake->click_user);
    CHECK(g_back_requests == 3 && g_clicks == 3);
    CHECK(components->nav_button_set_enabled(nav, 1).ok == kQolOk);
    fake->click(fake->click_user);
    CHECK(g_back_requests == 4 && g_clicks == 4);
    CHECK(components->nav_button_set_enabled(nav, 2).code == kQolErrInvalidArgument);

    // kind 隔离：close 入口作用于 nav 句柄、nav 入口作用于非 nav 句柄必须拒绝。
    CHECK(components->close_button_draw(nav).code == kQolErrUnsupported);
    CHECK(components->text_button_set_enabled(nav, 0).code == kQolErrUnsupported);
    CHECK(components->nav_button_draw(root).code == kQolErrUnsupported);

    // 主线程外绘制拒绝。
    g_fake_main_thread = 0;
    CHECK(components->nav_button_draw(nav).code == kQolErrNotReady);
    g_fake_main_thread = 1;

    // 裸原生父节点（由 adapter create_control 直接创建、未注册进 g_components）：
    // settings 页的 g_root 就是这种形态，nav 必须能挂上去（组件挂 g_roots，native 仍以该父为父）。
    QolUiHandle native_parent = nullptr;
    CHECK(create_control(0, nullptr, &native_parent).ok == kQolOk);
    CHECK(native_parent != nullptr);
    const qol::contracts::UiNavButtonDesc foreign_desc{
        {8, 8, 74, 81}, qol::contracts::kUiNavButtonAuto, &on_click, nullptr};
    g_option_image_loads = 0;
    QolUiHandle foreign_nav = nullptr;
    CHECK(components->nav_button_create(&foreign_desc, native_parent, &foreign_nav).ok == kQolOk);
    CHECK(foreign_nav != nullptr);
    // 创建即借入共享 option 图集（0x59），返回箭头分支不再静默失败。
    CHECK(g_option_image_loads == 1);
    g_fake_depth = 1;
    g_close_draws = 0;
    g_back_draws = 0;
    CHECK(components->nav_button_draw(foreign_nav).ok == kQolOk);
    CHECK(g_close_draws == 1 && g_back_draws == 0);
    g_fake_depth = 2;
    CHECK(components->nav_button_draw(foreign_nav).ok == kQolOk);
    CHECK(g_back_draws == 1 && g_back_handle == foreign_nav);
    g_fake_depth = 1;
    g_nav_deletes = 0;
    CHECK(components->destroy(foreign_nav).ok == kQolOk);
    CHECK(g_nav_deletes == 1);
    // 未放宽其它组件的 not_found 语义：非 nav 组件挂裸原生父节点仍拒绝。
    const qol::contracts::UiButtonDesc foreign_button_desc{
        {0, 0, 48, 48}, "x", 0, nullptr, nullptr};
    QolUiHandle foreign_button = nullptr;
    CHECK(components->button_create(&foreign_button_desc, native_parent, &foreign_button).code ==
          kQolErrNotFound);
    CHECK(foreign_button == nullptr);

    // 销毁：owned native 删除恰一次；重复销毁句柄已失效。
    g_nav_deletes = 0;
    CHECK(components->destroy(nav).ok == kQolOk);
    CHECK(g_nav_deletes == 1);
    CHECK(components->destroy(nav).code == kQolErrNotFound);

    CHECK(components->destroy(root).ok == kQolOk);
    qol::features::core_ui::ui_components_unbind();

    std::printf("nav_button_component assertions: %d\n", assertions);
    return 0;
#undef CHECK
}
