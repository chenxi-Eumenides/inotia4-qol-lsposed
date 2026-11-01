#include "plugins/core.custom_ui/ui_option_group.h"

#include <cstddef>
#include <cstdint>

#include "contracts/ui_theme.h"
#include "plugins/core.custom_ui/ui_components.h"

namespace qol::features::core_ui {
namespace {

constexpr int32_t kRowGap = 8;
constexpr int32_t kMinRowHeight = 24;

void option_noop(void*) {}

void option_selected(void* user, int32_t index) {
    auto* group = static_cast<UiOptionGroup*>(user);
    if (group == nullptr || index < 0 || index > 1) return;
    group->selected = index;
    if (group->on_change != nullptr) group->on_change(group->user, index);
}

void option0_clicked(void* user) { option_selected(user, 0); }
void option1_clicked(void* user) { option_selected(user, 1); }

uint32_t channel(uint32_t color, uint32_t shift) {
    return (color >> shift) & 0xFFu;
}

void draw_row(const contracts::QolUiAdapter* adapter, contracts::UiRect rect, bool selected,
              bool pressed, const char* text) {
    if (adapter == nullptr) return;
    if (adapter->draw_help_option_row != nullptr) {
        (void)adapter->draw_help_option_row(rect, selected ? 1u : 0u, pressed ? 1u : 0u);
    }
    if (adapter->set_font_color_rgb != nullptr && adapter->draw_string_with_font != nullptr &&
        text != nullptr) {
        (void)adapter->set_font_color_rgb(channel(contracts::ui_theme::kTextGold, 0),
                                          channel(contracts::ui_theme::kTextGold, 8),
                                          channel(contracts::ui_theme::kTextGold, 16));
        const int32_t y = rect.y + (rect.h > 22 ? (rect.h - 22) / 2 : 0);
        (void)adapter->draw_string_with_font(text, rect.x + 16, y, 0, 0);
    }
}

}  // namespace

QolResult option_group_create(const contracts::QolUiComponents* components,
                              contracts::QolUiHandle parent,
                              const UiOptionGroupDesc* desc, UiOptionGroup* out) {
    if (components == nullptr || desc == nullptr || out == nullptr || parent == nullptr ||
        components->button_create == nullptr || desc->labels[0] == nullptr ||
        desc->labels[1] == nullptr || desc->rect.w <= 0 || desc->rect.h <= kRowGap ||
        desc->selected < 0 || desc->selected > 1) {
        return contracts::error(kQolErrInvalidArgument, "invalid option group", 19);
    }
    const contracts::QolUiAdapter* adapter = ui_adapter();
    if (adapter == nullptr || adapter->draw_help_option_row == nullptr ||
        adapter->set_draw_proc == nullptr || adapter->control_pressed == nullptr) {
        return contracts::error(kQolErrNotReady, "option row api unavailable", 25);
    }
    const int32_t row_h = (desc->rect.h - kRowGap) / 2;
    if (row_h < kMinRowHeight) {
        return contracts::error(kQolErrInvalidArgument, "option rows too small", 22);
    }
    *out = {};
    out->labels[0] = desc->labels[0];
    out->labels[1] = desc->labels[1];
    out->on_change = desc->on_change;
    out->user = desc->user;
    out->selected = desc->selected;
    out->rects[0] = {desc->rect.x, desc->rect.y, desc->rect.w, row_h};
    out->rects[1] = {desc->rect.x, desc->rect.y + row_h + kRowGap, desc->rect.w, row_h};
    for (int32_t i = 0; i < 2; ++i) {
        contracts::UiButtonDesc button{out->rects[i], out->labels[i], 0,
                                       i == 0 ? &option0_clicked : &option1_clicked, out};
        contracts::QolUiHandle handle = nullptr;
        if (!contracts::succeeded(components->button_create(&button, parent, &handle)) ||
            handle == nullptr) {
            option_group_destroy(components, out);
            return contracts::error(kQolErrInternal, "option group create failed", 26);
        }
        out->handles[i] = handle;
        (void)adapter->set_draw_proc(handle, &option_noop);
    }
    return contracts::ok();
}

void option_group_draw(const contracts::QolUiAdapter* adapter, const UiOptionGroup* group) {
    if (adapter == nullptr || group == nullptr) return;
    for (int32_t i = 0; i < 2; ++i) {
        int32_t pressed = 0;
        if (group->handles[i] != nullptr && adapter->control_pressed != nullptr) {
            (void)adapter->control_pressed(group->handles[i], &pressed);
        }
        draw_row(adapter, group->rects[i], group->selected == i, pressed != 0, group->labels[i]);
    }
}

void option_group_destroy(const contracts::QolUiComponents* components, UiOptionGroup* group) {
    if (components == nullptr || group == nullptr) return;
    for (auto& handle : group->handles) {
        if (handle != nullptr && components->destroy != nullptr) {
            (void)components->destroy(handle);
        }
        handle = nullptr;
    }
    group->rects[0] = {};
    group->rects[1] = {};
    group->labels[0] = nullptr;
    group->labels[1] = nullptr;
    group->selected = 0;
}

}  // namespace qol::features::core_ui
