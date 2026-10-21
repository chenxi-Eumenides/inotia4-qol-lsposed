#include "ui_confirmation_overlay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace qol::plugins::custom_ui {
namespace {
using contracts::UiRect;

constexpr int64_t kMinPanelWidth = 280;
constexpr int64_t kMinPanelHeight = 184;
constexpr uint32_t kMaskColor = 0x0000;
constexpr uint32_t kMaskAlpha = 58;
constexpr uint32_t kPanelColor = 0x1082;
constexpr uint32_t kPanelAlpha = 100;
constexpr uint32_t kPanelEdge = 0x7BEF;
constexpr uint32_t kButtonColor = 0x2104;
constexpr uint32_t kConfirmColor = 0x5A80;
constexpr uint32_t kButtonEdge = 0xA514;
constexpr uint32_t kTextColor = 0xFFFF;
constexpr uint32_t kPriceColor = 0xFFE0;
constexpr int64_t kButtonHeight = 44;
constexpr int64_t kInset = 24;
constexpr int64_t kGap = 14;

bool positive(UiRect r) { return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0; }

bool inside(UiRect inner, UiRect outer) {
    return positive(inner) && positive(outer) && inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;
}

bool overlaps(UiRect a, UiRect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

bool equal(UiRect a, UiRect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool layout_valid(const UiConfirmationOverlayLayout& l, bool price) {
    if (l.valid == 0 || l.screen_w == 0 || l.screen_h == 0 ||
        !inside(l.panel, {0, 0, l.screen_w, l.screen_h}) ||
        !inside(l.text, l.panel) || !inside(l.confirm_button, l.panel) ||
        !inside(l.cancel_button, l.panel) || overlaps(l.confirm_button, l.cancel_button) ||
        overlaps(l.text, l.confirm_button) || overlaps(l.text, l.cancel_button) ||
        (price != (l.has_price != 0)) ||
        (price ? (!inside(l.price, l.panel) || overlaps(l.price, l.text) ||
                  overlaps(l.price, l.confirm_button) || overlaps(l.price, l.cancel_button)) :
                 positive(l.price))) return false;
    const UiRect expected_masks[] = {
        {0, 0, l.screen_w, l.panel.y},
        {0, l.panel.y + l.panel.h, l.screen_w,
         static_cast<int64_t>(l.screen_h) - l.panel.y - l.panel.h},
        {0, l.panel.y, l.panel.x, l.panel.h},
        {l.panel.x + l.panel.w, l.panel.y,
         static_cast<int64_t>(l.screen_w) - l.panel.x - l.panel.w, l.panel.h},
    };
    const UiRect actual_masks[] = {l.mask_top, l.mask_bottom, l.mask_left, l.mask_right};
    for (size_t i = 0; i < 4; ++i) {
        if (!equal(actual_masks[i], expected_masks[i])) return false;
    }
    const UiRect masks[] = {l.mask_top, l.mask_bottom, l.mask_left, l.mask_right};
    for (const UiRect mask : masks) {
        if (mask.w == 0 || mask.h == 0) continue;
        if (!inside(mask, {0, 0, l.screen_w, l.screen_h}) || overlaps(mask, l.panel)) return false;
    }
    return true;
}

bool draw_rect(contracts::QolUiAdapter& ui, UiRect rect, uint32_t color, uint32_t alpha) {
    return rect.w == 0 || rect.h == 0 ||
           (ui.fill_rect_alpha != nullptr &&
            ui.fill_rect_alpha(rect, color, alpha).ok == ::kQolOk);
}

bool draw_text(contracts::QolUiAdapter& ui, const char* text, int32_t x, int32_t y,
               uint32_t color, int32_t align = 0) {
    if (ui.draw_string_with_font == nullptr || text == nullptr) return false;
    if (ui.set_font_color != nullptr && ui.set_font_color(color).ok != ::kQolOk) return false;
    return ui.draw_string_with_font(text, x, y, align, 0).ok == ::kQolOk;
}

size_t utf8_prefix(const char* text, size_t max_chars) {
    size_t bytes = 0;
    size_t chars = 0;
    while (text[bytes] != '\0' && chars < max_chars) {
        const unsigned char ch = static_cast<unsigned char>(text[bytes]);
        size_t width = ch < 0x80 ? 1 : (ch & 0xE0) == 0xC0 ? 2 : (ch & 0xF0) == 0xE0 ? 3 :
                       (ch & 0xF8) == 0xF0 ? 4 : 1;
        size_t i = 1;
        while (i < width && text[bytes + i] != '\0' &&
               (static_cast<unsigned char>(text[bytes + i]) & 0xC0) == 0x80) ++i;
        bytes += i;
        ++chars;
    }
    return bytes;
}

bool draw_message(contracts::QolUiAdapter& ui, const char* message, UiRect area) {
    const size_t max_chars = static_cast<size_t>(std::max<int64_t>(1, area.w / 15));
    const int64_t max_lines = std::max<int64_t>(1, area.h / 26);
    const char* cursor = message;
    int64_t line_no = 0;
    while (*cursor != '\0' && line_no < max_lines) {
        const char* newline = std::strchr(cursor, '\n');
        const size_t source_len = newline == nullptr ? std::strlen(cursor) :
                                  static_cast<size_t>(newline - cursor);
        std::string line(cursor, source_len);
        const size_t visible_bytes = utf8_prefix(line.c_str(), max_chars);
        if (visible_bytes < line.size()) line.resize(visible_bytes);
        if (!draw_text(ui, line.c_str(), static_cast<int32_t>(area.x + 4),
                       static_cast<int32_t>(area.y + line_no * 26), kTextColor)) return false;
        ++line_no;
        cursor = newline == nullptr ? cursor + source_len : newline + 1;
    }
    // draw_string_with_font has no width/clip parameter, so long lines are UTF-8-safely
    // truncated to a conservative character budget; explicit newlines create additional rows.
    return true;
}

}  // namespace

bool ui_confirmation_overlay_layout(uint32_t screen_w, uint32_t screen_h,
                                    const UiConfirmationOverlayContent& content,
                                    UiConfirmationOverlayLayout& out) {
    out = {};
    if (screen_w == 0 || screen_h == 0 || content.message == nullptr ||
        content.price > static_cast<uint32_t>(INT32_MAX)) return false;
    const int64_t sw = screen_w, sh = screen_h;
    const bool horizontal = sw >= 560 && sw * 10 >= sh * 13;
    const int64_t panel_w = std::min<int64_t>(760, sw * 84 / 100);
    const int64_t panel_h = std::min<int64_t>(360, sh * 78 / 100);
    if (panel_w < kMinPanelWidth || panel_h < kMinPanelHeight) return false;
    const int64_t px = (sw - panel_w) / 2, py = (sh - panel_h) / 2;
    const int64_t button_area_w = panel_w - 2 * kInset;
    const int64_t button_y = py + panel_h - kInset - kButtonHeight;
    UiRect confirm{}, cancel{};
    if (horizontal) {
        const int64_t button_w = (button_area_w - kGap) / 2;
        confirm = {px + kInset, button_y, button_w, kButtonHeight};
        cancel = {confirm.x + button_w + kGap, button_y, button_w, kButtonHeight};
    } else {
        const int64_t button_w = button_area_w;
        confirm = {px + kInset, button_y - kButtonHeight - 8, button_w, kButtonHeight};
        cancel = {px + kInset, button_y, button_w, kButtonHeight};
    }
    const int64_t content_top = py + 28;
    const int64_t content_bottom = horizontal ? button_y - 18 : confirm.y - 14;
    const int64_t price_h = content.has_price ? 28 : 0;
    if (content_bottom - content_top - price_h < 28) return false;
    out.panel = {px, py, panel_w, panel_h};
    out.text = {px + kInset, content_top, panel_w - 2 * kInset, content_bottom - content_top - price_h};
    out.price = content.has_price ? UiRect{px + kInset, content_bottom - price_h,
                                            panel_w - 2 * kInset, price_h} : UiRect{};
    out.confirm_button = confirm;
    out.cancel_button = cancel;
    out.screen_w = screen_w;
    out.screen_h = screen_h;
    out.has_price = content.has_price ? 1 : 0;
    out.mask_top = {0, 0, sw, py};
    out.mask_bottom = {0, py + panel_h, sw, sh - py - panel_h};
    out.mask_left = {0, py, px, panel_h};
    out.mask_right = {px + panel_w, py, sw - px - panel_w, panel_h};
    out.valid = 1;
    if (!layout_valid(out, content.has_price)) { out = {}; return false; }
    return true;
}

bool ui_confirmation_overlay_draw(contracts::QolUiAdapter& ui,
                                  const UiConfirmationOverlayContent& content,
                                  const UiConfirmationOverlayLayout& layout) {
    if (content.message == nullptr || !layout_valid(layout, content.has_price) ||
        ui.fill_rect_alpha == nullptr || ui.draw_string_with_font == nullptr) return false;
    if (!draw_rect(ui, layout.mask_top, kMaskColor, kMaskAlpha) ||
        !draw_rect(ui, layout.mask_bottom, kMaskColor, kMaskAlpha) ||
        !draw_rect(ui, layout.mask_left, kMaskColor, kMaskAlpha) ||
        !draw_rect(ui, layout.mask_right, kMaskColor, kMaskAlpha) ||
        !draw_rect(ui, layout.panel, kPanelColor, kPanelAlpha)) return false;
    // One-pixel inset edge avoids compositing a translucent border over the panel fill.
    const UiRect p = layout.panel;
    if (!draw_rect(ui, {p.x, p.y, p.w, 2}, kPanelEdge, 100) ||
        !draw_rect(ui, {p.x, p.y + p.h - 2, p.w, 2}, kPanelEdge, 100) ||
        !draw_rect(ui, {p.x, p.y + 2, 2, p.h - 4}, kPanelEdge, 100) ||
        !draw_rect(ui, {p.x + p.w - 2, p.y + 2, 2, p.h - 4}, kPanelEdge, 100) ||
        !draw_rect(ui, layout.confirm_button, kConfirmColor, 100) ||
        !draw_rect(ui, layout.cancel_button, kButtonColor, 100)) return false;
    const UiRect confirm = layout.confirm_button, cancel = layout.cancel_button;
    if (!draw_rect(ui, {confirm.x, confirm.y, confirm.w, 2}, kButtonEdge, 100) ||
        !draw_rect(ui, {cancel.x, cancel.y, cancel.w, 2}, kButtonEdge, 100)) return false;
    if (!draw_message(ui, content.message, layout.text)) return false;
    if (content.has_price) {
        char amount[32]{};
        std::snprintf(amount, sizeof(amount), "%u", content.price);
        std::string price_text = amount;
        if (content.price_unit != nullptr && content.price_unit[0] != '\0') {
            price_text.push_back(' ');
            price_text += content.price_unit;
        }
        if (!draw_text(ui, price_text.c_str(), static_cast<int32_t>(layout.price.x + 4),
                       static_cast<int32_t>(layout.price.y), kPriceColor)) return false;
    }
    const char* confirm_label = content.confirm_label == nullptr ? "确认" : content.confirm_label;
    const char* cancel_label = content.cancel_label == nullptr ? "取消" : content.cancel_label;
    return draw_text(ui, confirm_label, static_cast<int32_t>(confirm.x + confirm.w / 2),
                     static_cast<int32_t>(confirm.y + 11), kTextColor, 2) &&
           draw_text(ui, cancel_label, static_cast<int32_t>(cancel.x + cancel.w / 2),
                     static_cast<int32_t>(cancel.y + 11), kTextColor, 2);
}

UiConfirmationOverlayHit ui_confirmation_overlay_hit_test(
    const UiConfirmationOverlayLayout& layout, int32_t x, int32_t y) {
    if (layout.valid == 0) return UiConfirmationOverlayHit::kNone;
    const auto contains = [x, y](UiRect r) {
        return x >= r.x && y >= r.y && x <= r.x + r.w && y <= r.y + r.h;
    };
    if (contains(layout.confirm_button)) return UiConfirmationOverlayHit::kConfirm;
    if (contains(layout.cancel_button)) return UiConfirmationOverlayHit::kCancel;
    return UiConfirmationOverlayHit::kNone;
}

}  // namespace qol::plugins::custom_ui
