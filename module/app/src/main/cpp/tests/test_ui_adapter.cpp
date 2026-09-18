#include <cassert>
#include <cstdint>
#include <cstdio>

#include "contracts/ui.h"
#include "game/access/ui_kit.h"
#include "host/ui_adapter.h"

namespace {

struct FakeControl {
    UiRect rect{};
};

int g_set_rect_calls = 0;
int g_get_rect_calls = 0;
int g_hit_test_calls = 0;
int g_fill_rect_calls = 0;

void draw_proc(void*) {}

}  // namespace

void* ui_create_root(UiRect) { return nullptr; }
void* ui_create_button(void*, UiRect, const char*, UiClickProc, UiDrawProc) { return nullptr; }
void* ui_create_label(void*, UiRect, const char*, UiDrawProc) { return nullptr; }

void ui_set_rect(void* ctrl, UiRect rect) {
    ++g_set_rect_calls;
    static_cast<FakeControl*>(ctrl)->rect = rect;
}

bool ui_get_rect(void* ctrl, UiRect* rect) {
    ++g_get_rect_calls;
    if (ctrl == nullptr || rect == nullptr) return false;
    *rect = static_cast<FakeControl*>(ctrl)->rect;
    return true;
}

bool ui_hit_test(void* ctrl, int64_t x, int64_t y, UiRect rect) {
    ++g_hit_test_calls;
    if (ctrl == nullptr) return false;
    const UiRect& origin = static_cast<FakeControl*>(ctrl)->rect;
    return x >= origin.x && x < origin.x + rect.w && y >= origin.y && y < origin.y + rect.h;
}

void ui_begin_frame() {}
void ui_fill_rect_alpha(UiRect, uint32_t, uint32_t) { ++g_fill_rect_calls; }
void ui_end_frame() {}
void ui_draw_panel_decor(UiRect, const int64_t*, size_t, uint32_t) {}
void ui_draw_vertical_line(int64_t, int64_t, int64_t, uint32_t, int) {}
void ui_draw_text(void*, int, int, uint32_t) {}
void ui_draw_text_centered(void*, int, uint32_t) {}
bool ui_load_image_unit(int32_t) { return true; }
void ui_unload_image_unit(int32_t) {}
bool ui_draw_image_part(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) { return true; }
bool ui_draw_control_image_part(void*, int32_t, int32_t, int32_t, int32_t) { return true; }
bool ui_draw_control_image_part_centered(void*, int32_t, int32_t, int32_t, int32_t) { return true; }
void ui_draw_button_background(void*, UiRect, uint32_t) {}
void ui_draw_button_border(void*, UiRect, uint32_t, int) {}
void ui_draw_control_alpha_overlay(void*, UiRect, uint32_t, uint32_t) {}

int main() {
    int assertions = 0;
#define CHECK(condition) \
    do {                  \
        assert(condition); \
        ++assertions;     \
    } while (false)

    const qol::contracts::QolUiAdapter* adapter = qol::host::ui_adapter();
    CHECK(adapter != nullptr);
    CHECK(adapter->size == sizeof(qol::contracts::QolUiAdapter));
    CHECK(adapter->major == qol::contracts::kUiContractMajor);
    CHECK(adapter->minor == qol::contracts::kUiContractMinor);

    qol::contracts::QolUiComponents components{};
    components.size = sizeof(components);
    components.major = qol::contracts::kUiContractMajor;
    components.minor = qol::contracts::kUiContractMinor;
    CHECK(components.size == sizeof(qol::contracts::QolUiComponents));
    CHECK(components.major == 1 && components.minor == 0);

    const qol::contracts::UiRect rect{10, 20, 30, 40};
    FakeControl control{};
    CHECK(adapter->set_rect(&control, rect).ok == kQolOk);
    CHECK(g_set_rect_calls == 1);

    qol::contracts::UiRect out{};
    CHECK(adapter->get_rect(&control, &out).ok == kQolOk);
    CHECK(g_get_rect_calls == 1);
    CHECK(out.x == 10 && out.y == 20 && out.w == 30 && out.h == 40);

    CHECK(adapter->hit_test(&control, 10, 20, rect) == 1);
    CHECK(adapter->hit_test(&control, 40, 20, rect) == 0);
    CHECK(g_hit_test_calls == 2);

    CHECK(adapter->fill_rect_alpha(rect, 0x123456, 0x50).ok == kQolOk);
    CHECK(g_fill_rect_calls == 1);

    CHECK(adapter->set_rect(nullptr, rect).code == kQolErrInvalidArgument);
    CHECK(adapter->get_rect(&control, nullptr).code == kQolErrInvalidArgument);
    CHECK(adapter->draw_text(nullptr, 0, 0, 0).code == kQolErrInvalidArgument);
    qol::contracts::QolUiHandle created = nullptr;
    CHECK(adapter->create_control(1, nullptr, &created).code == kQolErrInvalidArgument);
    CHECK(adapter->set_text(&control, nullptr).code == kQolErrInvalidArgument);
    CHECK(adapter->set_text(&control, "text").code == kQolErrUnsupported);
    CHECK(adapter->set_draw_proc(&control, &draw_proc).code == kQolErrUnsupported);
    CHECK(adapter->draw_control(&control).code == kQolErrUnsupported);

    std::printf("ui_adapter assertions: %d\n", assertions);
    return 0;
#undef CHECK
}
