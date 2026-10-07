#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "contracts/ui.h"
#include "plugins/core.custom_ui/ui_components.h"

// 返回按钮助手内部调用宿主 back 入口；本测试不链接面板栈实现。
namespace qol::features::core_ui {
void ui_panels_request_back() {}
int ui_panels_depth() { return 1; }
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
int g_close_draws = 0;
int32_t g_close_pressed = -1;
qol::contracts::UiRect g_close_rect{};
int g_fake_main_thread = 1;


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

QolResult draw_back_button(QolUiHandle, uint8_t) { return ok(); }

QolResult control_pressed(qol::contracts::QolUiHandle handle, int32_t* out) {
    if (handle == nullptr || out == nullptr) return invalid();
    *out = static_cast<FakeControl*>(handle)->pressed;
    return ok();
}

QolResult control_absolute_origin(qol::contracts::QolUiHandle handle, int64_t* out_x,
                                  int64_t* out_y) {
    if (handle == nullptr || out_x == nullptr || out_y == nullptr) return invalid();
    *out_x = 0;
    *out_y = 0;
    return ok();
}

// 模拟引擎事件树语义：按下落在按钮内置 enabled，抬起仍在按钮内且 enabled 才触发点击；
// 移出后抬起取消。与 nav 组件测试的同款 fake 保持一致。
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
    return ok();
}

QolResult load_option_images() { return ok(); }

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
    using qol::contracts::QolUiComponents;

    // 表尾纯追加：既有成员保持原偏移，nav 三指针位于其后且可用。
    static_assert(offsetof(QolUiComponents, nav_button_create) >
        offsetof(QolUiComponents, text_button_draw), "nav entries must append the table");
    const QolUiComponents* components = qol::features::core_ui::ui_components_api();
    assert(components->text_button_create != nullptr && components->text_button_draw != nullptr);
    assert(components->nav_button_create != nullptr &&
           components->nav_button_set_enabled != nullptr &&
           components->nav_button_draw != nullptr);
    assert(components->major == qol::contracts::kUiContractMajor &&
           components->minor == qol::contracts::kUiContractMinor);

    QolUiAdapter adapter = make_adapter();
    assert(qol::features::core_ui::ui_components_bind(&adapter));
    QolUiHandle root = nullptr;
    const qol::contracts::UiPanelDesc panel_desc{{0, 0, 480, 320}, {0, 0, 480, 320}, 0, 0};
    assert(components->panel_create(&panel_desc, nullptr, &root).ok == kQolOk);

    // nav 关闭形态（ForceClose）：X 路径的组件级验证——原生事件树命中、按下态透传与
    // 绘制 rect 与独立关闭按钮组件同语义，只是创建入口与视觉模式归 nav 组件。
    QolUiHandle close = nullptr;
    const qol::contracts::UiNavButtonDesc valid_desc{
        {16, 16, 48, 48}, qol::contracts::kUiNavButtonForceClose, &on_click, nullptr};

    // 旧 minor（15）adapter 可通过 bind（基线 13），但 nav_button_create 要求 minor 16：
    // 旧前缀消费者不受影响，新组件在新表协商失败时返回 not_ready。
    {
        QolUiAdapter old_adapter = make_adapter();
        old_adapter.minor = 15;
        assert(qol::features::core_ui::ui_components_bind(&old_adapter));
        QolUiHandle stale = nullptr;
        assert(components->nav_button_create(&valid_desc, root, &stale).code == kQolErrNotReady);
        assert(stale == nullptr);
        qol::features::core_ui::ui_components_bind(&adapter);
    }

    assert(components->nav_button_create(nullptr, root, &close).code == kQolErrInvalidArgument);
    assert(close == nullptr);
    const qol::contracts::UiNavButtonDesc bad_mode{
        {16, 16, 48, 48}, 3, &on_click, nullptr};
    assert(components->nav_button_create(&bad_mode, root, &close).code ==
           kQolErrInvalidArgument);
    const qol::contracts::UiNavButtonDesc flat_rect{
        {16, 16, 0, 48}, qol::contracts::kUiNavButtonForceClose, &on_click, nullptr};
    assert(components->nav_button_create(&flat_rect, root, &close).code ==
           kQolErrInvalidArgument);
    // nav 按钮为非 panel 组件：parent 为空直接 invalid_argument。
    assert(components->nav_button_create(&valid_desc, nullptr, &close).code ==
           kQolErrInvalidArgument);

    assert(components->nav_button_create(&valid_desc, root, &close).ok == kQolOk);
    assert(close != nullptr);

    // 绘制：常态一次、按下态读 control_pressed；栈深不影响 ForceClose 选图。
    assert(components->nav_button_draw(close).ok == kQolOk);
    assert(g_close_draws == 1 && g_close_pressed == 0);
    assert(g_close_rect.x == 0 && g_close_rect.y == 0 && g_close_rect.w == 48 &&
           g_close_rect.h == 48);
    static_cast<FakeControl*>(close)->pressed = 1;
    assert(components->nav_button_draw(close).ok == kQolOk);
    assert(g_close_draws == 2 && g_close_pressed == 1);
    static_cast<FakeControl*>(close)->pressed = 0;

    // 点击恰一次；禁用后点击不触发；重新启用后恢复。
    assert(g_clicks == 0);
    auto* fake = static_cast<FakeControl*>(close);
    fake->click(fake->click_user);
    assert(g_clicks == 1);
    assert(components->nav_button_set_enabled(close, 0).ok == kQolOk);
    fake->click(fake->click_user);
    assert(g_clicks == 1);
    assert(components->nav_button_set_enabled(close, 1).ok == kQolOk);
    fake->click(fake->click_user);
    assert(g_clicks == 2);
    assert(components->nav_button_set_enabled(close, 2).code == kQolErrInvalidArgument);

    // 原生事件树路径：按下内部+抬起内部 → 恰一次；按下后移出抬起 → 取消；外部按下不触发。
    int64_t inside_point[2] = {20, 20};
    int64_t outside_point[2] = {400, 400};
    uint32_t consumed = 0;
    assert(components->dispatch_native_event(close, 0x17,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside_point)), 0, &consumed).ok ==
        kQolOk);
    assert(components->dispatch_native_event(close, 0x18,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside_point)), 0, &consumed).ok ==
        kQolOk);
    assert(g_clicks == 3);
    assert(components->dispatch_native_event(close, 0x17,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside_point)), 0, &consumed).ok ==
        kQolOk);
    assert(components->dispatch_native_event(close, 0x18,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(outside_point)), 0, &consumed).ok ==
        kQolOk);
    assert(g_clicks == 3);
    assert(components->dispatch_native_event(close, 0x17,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(outside_point)), 0, &consumed).ok ==
        kQolOk);
    assert(components->dispatch_native_event(close, 0x18,
        reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(inside_point)), 0, &consumed).ok ==
        kQolOk);
    assert(g_clicks == 3);

    // kind 隔离：text_button 入口作用于 nav 句柄必须拒绝。
    assert(components->text_button_set_enabled(close, 0).code == kQolErrUnsupported);

    // 主线程外绘制拒绝。
    g_fake_main_thread = 0;
    assert(components->nav_button_draw(close).code == kQolErrNotReady);
    g_fake_main_thread = 1;

    // 销毁后句柄失效。
    assert(components->destroy(close).ok == kQolOk);
    assert(components->nav_button_draw(close).code == kQolErrNotFound);

    qol::features::core_ui::ui_components_unbind();
    return 0;
}
