#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "plugins/core.custom_ui/ui_confirmation_overlay.h"

namespace {
using namespace qol;
using namespace qol::plugins::custom_ui;

struct Call {
    bool text;
    contracts::UiRect rect;
    std::string label;
};
std::vector<Call> g_calls;

QolResult fake_fill(contracts::UiRect rect, uint32_t, uint32_t) {
    g_calls.push_back({false, rect, {}});
    return contracts::ok();
}
QolResult fake_text(const char* text, int32_t, int32_t, int32_t, int32_t) {
    g_calls.push_back({true, {}, text == nullptr ? "" : text});
    return contracts::ok();
}
QolResult fake_color(uint32_t) { return contracts::ok(); }

contracts::QolUiAdapter adapter() {
    contracts::QolUiAdapter ui{};
    ui.fill_rect_alpha = fake_fill;
    ui.draw_string_with_font = fake_text;
    ui.set_font_color = fake_color;
    return ui;
}

bool has_text(const char* text) {
    for (const auto& call : g_calls) if (call.text && call.label == text) return true;
    return false;
}
bool overlaps(contracts::UiRect a, contracts::UiRect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

void test_default_and_custom_labels() {
    auto ui = adapter();
    UiConfirmationOverlayContent content{"是否购买？", nullptr, nullptr, false, 0, nullptr};
    UiConfirmationOverlayLayout layout{};
    assert(ui_confirmation_overlay_layout(900, 600, content, layout));
    assert(ui_confirmation_overlay_draw(ui, content, layout));
    assert(has_text("确认") && has_text("取消"));
    assert(!has_text("0"));
    size_t text_calls = 0;
    for (const auto& call : g_calls) if (call.text) ++text_calls;
    assert(text_calls == 3);  // Message and two labels; no price line.
    g_calls.clear();
    content.confirm_label = "接受";
    content.cancel_label = "返回";
    assert(ui_confirmation_overlay_draw(ui, content, layout));
    assert(has_text("接受") && has_text("返回"));
}

void test_price_and_hit_bounds() {
    auto ui = adapter();
    UiConfirmationOverlayContent content{"购买物品", "买", "不买", true, 125, "G"};
    UiConfirmationOverlayLayout layout{};
    assert(ui_confirmation_overlay_layout(900, 600, content, layout));
    assert(ui_confirmation_overlay_draw(ui, content, layout));
    assert(has_text("125 G"));
    const auto check_corners = [](contracts::UiRect r, UiConfirmationOverlayHit expected) {
        assert(ui_confirmation_overlay_hit_test({}, 0, 0) == UiConfirmationOverlayHit::kNone);
        UiConfirmationOverlayLayout l{};
        l.valid = 1;
        if (expected == UiConfirmationOverlayHit::kConfirm) l.confirm_button = r;
        else l.cancel_button = r;
        const int32_t left = static_cast<int32_t>(r.x);
        const int32_t top = static_cast<int32_t>(r.y);
        const int32_t right = static_cast<int32_t>(r.x + r.w);
        const int32_t bottom = static_cast<int32_t>(r.y + r.h);
        assert(ui_confirmation_overlay_hit_test(l, left, top) == expected);
        assert(ui_confirmation_overlay_hit_test(l, right, top) == expected);
        assert(ui_confirmation_overlay_hit_test(l, left, bottom) == expected);
        assert(ui_confirmation_overlay_hit_test(l, right, bottom) == expected);
        assert(ui_confirmation_overlay_hit_test(l, static_cast<int32_t>(r.x + r.w / 2),
                 static_cast<int32_t>(r.y + r.h / 2)) == expected);
    };
    check_corners(layout.confirm_button, UiConfirmationOverlayHit::kConfirm);
    check_corners(layout.cancel_button, UiConfirmationOverlayHit::kCancel);
}

void test_failed_layout_is_noop_and_masks_do_not_overlap() {
    auto ui = adapter();
    UiConfirmationOverlayContent content{"too small", nullptr, nullptr, false, 0, nullptr};
    UiConfirmationOverlayLayout layout{};
    assert(!ui_confirmation_overlay_layout(200, 120, content, layout));
    assert(!ui_confirmation_overlay_draw(ui, content, layout));
    assert(g_calls.empty());
    assert(ui_confirmation_overlay_layout(900, 600, content, layout));
    const contracts::UiRect masks[] = {
        layout.mask_top, layout.mask_bottom, layout.mask_left, layout.mask_right};
    for (auto mask : masks) if (mask.w > 0 && mask.h > 0) assert(!overlaps(mask, layout.panel));
}

}  // namespace

int main() {
    test_default_and_custom_labels();
    g_calls.clear();
    test_price_and_hit_bounds();
    g_calls.clear();
    test_failed_layout_is_noop_and_masks_do_not_overlap();
    return 0;
}
