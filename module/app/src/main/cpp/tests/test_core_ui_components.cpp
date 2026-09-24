#include <cassert>
#include <cstring>
#include <cstdio>

#include "contracts/ui.h"
#include "plugins/core.custom_ui/ui_components.h"

namespace {

struct FakeControl {
    uint32_t kind = 0;
    FakeControl* parent = nullptr;
    qol::contracts::UiRect rect{};
    char text[64] = {};
    qol::contracts::QolUiClickFn click = nullptr;
    void* click_user = nullptr;
    qol::contracts::QolUiDrawFn draw = nullptr;
};

int g_clicks = 0;
int g_changes = 0;
int g_scrolls = 0;
int32_t g_last_value = -1;
int32_t g_last_offset = -1;

void on_click(void* user) {
    assert(user != nullptr);
    ++g_clicks;
}

void on_changed(void*, int32_t value) {
    ++g_changes;
    g_last_value = value;
}

void on_scroll(void*, int32_t offset) {
    ++g_scrolls;
    g_last_offset = offset;
}

QolResult ok() { return qol::contracts::ok(); }

QolResult create_control(uint32_t kind, qol::contracts::QolUiHandle parent,
                         qol::contracts::QolUiHandle* out) {
    if (out == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    auto* control = new FakeControl();
    control->kind = kind;
    control->parent = static_cast<FakeControl*>(parent);
    *out = control;
    return ok();
}

QolResult set_rect(qol::contracts::QolUiHandle handle, qol::contracts::UiRect rect) {
    if (handle == nullptr) return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    static_cast<FakeControl*>(handle)->rect = rect;
    return ok();
}

QolResult set_text(qol::contracts::QolUiHandle handle, const char* text) {
    if (handle == nullptr || text == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    std::snprintf(static_cast<FakeControl*>(handle)->text,
                  sizeof(static_cast<FakeControl*>(handle)->text), "%s", text);
    return ok();
}

QolResult set_draw_proc(qol::contracts::QolUiHandle handle, qol::contracts::QolUiDrawFn draw) {
    if (handle == nullptr || draw == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    static_cast<FakeControl*>(handle)->draw = draw;
    return ok();
}

QolResult set_click_proc(qol::contracts::QolUiHandle handle, qol::contracts::QolUiClickFn click,
                         void* user) {
    if (handle == nullptr || click == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid", 7);
    }
    auto* control = static_cast<FakeControl*>(handle);
    control->click = click;
    control->click_user = user;
    return ok();
}

int32_t hit_test(const qol::contracts::QolUiHandle handle, int64_t x, int64_t y,
                 qol::contracts::UiRect rect) {
    if (handle == nullptr) return -kQolErrInvalidArgument;
    const auto* control = static_cast<const FakeControl*>(handle);
    int64_t absolute_x = 0;
    int64_t absolute_y = 0;
    for (const FakeControl* current = control; current != nullptr; current = current->parent) {
        absolute_x += current->rect.x;
        absolute_y += current->rect.y;
    }
    return x >= absolute_x + rect.x && x < absolute_x + rect.x + rect.w &&
        y >= absolute_y + rect.y && y < absolute_y + rect.y + rect.h;
}

QolResult draw_control(qol::contracts::QolUiHandle) { return ok(); }

qol::contracts::QolUiAdapter make_fake_adapter() {
    qol::contracts::QolUiAdapter adapter{};
    adapter.size = sizeof(adapter);
    adapter.major = 1;
    adapter.minor = 0;
    adapter.create_control = &create_control;
    adapter.set_rect = &set_rect;
    adapter.set_text = &set_text;
    adapter.set_draw_proc = &set_draw_proc;
    adapter.set_click_proc = &set_click_proc;
    adapter.hit_test = &hit_test;
    adapter.draw_control = &draw_control;
    return adapter;
}

const qol::contracts::QolUiAdapter kFakeAdapter = make_fake_adapter();

}  // namespace

int main() {
    int assertions = 0;
#define CHECK(condition) \
    do {                    \
        assert(condition);  \
        ++assertions;       \
    } while (false)

    CHECK(qol::features::core_ui::ui_components_bind(&kFakeAdapter));
    const auto* components = qol::features::core_ui::ui_components_api();
    CHECK(components != nullptr);
    CHECK(components->size == sizeof(qol::contracts::QolUiComponents));
    CHECK(components->panel_create != nullptr && components->scroll_view_create != nullptr);

    qol::contracts::UiPanelDesc panel_desc{{100, 50, 100, 100}, {0, 0, 100, 100}, 0, 0};
    qol::contracts::QolUiHandle root = nullptr;
    CHECK(components->panel_create(&panel_desc, nullptr, &root).ok == kQolOk);

    char button_text[] = "button";
    qol::contracts::UiButtonDesc button_desc{{2, 3, 20, 10}, button_text, 7, &on_click, root};
    qol::contracts::QolUiHandle button = nullptr;
    CHECK(components->button_create(&button_desc, root, &button).ok == kQolOk);
    button_text[0] = 'X';
    CHECK(std::strcmp(static_cast<FakeControl*>(button)->text, "button") == 0);

    qol::contracts::UiLabelDesc label_desc{{40, 3, 20, 10}, "label", 0, qol::contracts::UiAlign::kLeft};
    qol::contracts::QolUiHandle label = nullptr;
    CHECK(components->label_create(&label_desc, root, &label).ok == kQolOk);

    qol::contracts::UiToggleDesc toggle_desc{{2, 30, 20, 10}, "toggle", 0, 0, &on_changed, nullptr};
    qol::contracts::QolUiHandle toggle = nullptr;
    CHECK(components->toggle_create(&toggle_desc, root, &toggle).ok == kQolOk);

    const char* options[] = {"one", "two"};
    qol::contracts::UiSelectDesc select_desc{{40, 30, 20, 10}, options, 2, 0, 0, &on_changed, nullptr};
    qol::contracts::QolUiHandle select = nullptr;
    CHECK(components->select_create(&select_desc, root, &select).ok == kQolOk);

    qol::contracts::UiScrollViewDesc scroll_desc{{70, 30, 20, 10}, 100, 10, 20, 0, &on_scroll, nullptr};
    qol::contracts::QolUiHandle scroll = nullptr;
    CHECK(components->scroll_view_create(&scroll_desc, root, &scroll).ok == kQolOk);

    CHECK(components->button_set_text(button, "updated").ok == kQolOk);
    CHECK(std::strcmp(static_cast<FakeControl*>(button)->text, "updated") == 0);
    CHECK(components->label_set_text(label, "updated label").ok == kQolOk);
    CHECK(components->set_rect(button, {4, 5, 20, 10}).ok == kQolOk);
    CHECK(components->toggle_set_value(toggle, 1).ok == kQolOk);
    CHECK(components->select_set_selected(select, 1).ok == kQolOk);
    CHECK(components->scroll_set_offset(scroll, 50).ok == kQolOk);
    CHECK(components->select_set_selected(select, 2).code == kQolErrInvalidArgument);
    CHECK(components->scroll_set_offset(scroll, 81).code == kQolErrInvalidArgument);

    CHECK(components->dispatch_touch(root, 0, 105, 58).ok == kQolOk);
    CHECK(g_clicks == 0);
    CHECK(components->dispatch_touch(root, 1, 105, 58).ok == kQolOk);
    CHECK(g_clicks == 1);
    CHECK(components->dispatch_touch(root, 1, 103, 83).ok == kQolOk);
    CHECK(g_changes == 1 && g_last_value == 0);
    CHECK(components->dispatch_touch(root, 1, 141, 83).ok == kQolOk);
    CHECK(g_changes == 2 && g_last_value == 0);
    CHECK(components->dispatch_touch(root, 1, 171, 83).ok == kQolOk);
    CHECK(g_scrolls == 1 && g_last_offset == 50);
    CHECK(components->dispatch_touch(root, 1, 300, 300).ok == kQolOk);

    CHECK(components->destroy(button).ok == kQolOk);
    CHECK(components->destroy(button).code == kQolErrNotFound);
    CHECK(components->destroy(nullptr).code == kQolErrInvalidArgument);
    CHECK(components->destroy(root).ok == kQolOk);
    CHECK(components->destroy(root).code == kQolErrNotFound);
    qol::features::core_ui::ui_components_unbind();

    std::printf("core_ui_components assertions: %d\n", assertions);
    return 0;
#undef CHECK
}
