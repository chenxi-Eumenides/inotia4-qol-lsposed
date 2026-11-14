#include "../plugin.h"

#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "../../../contracts/panels.h"
#include "../../../contracts/log_domain.h"

#include <cstdio>
#include <cstring>

namespace qol::features::item_editor {
namespace {
contracts::QolUiAnchoredButton g_entry{};

void log_info(const char* message) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || message == nullptr) return;
    (void)sdk->log.write(1, static_cast<int32_t>(QolDomain::kUi), "item_editor/entry", message,
                         static_cast<uint32_t>(std::strlen(message)));
}

const contracts::QolUiComponents* components() {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* value = nullptr;
    if (!contracts::succeeded(sdk->capabilities.require(contracts::kUiComponentsCapabilityId,
            {contracts::kUiContractMajor, contracts::kUiContractMinor}, &value))) return nullptr;
    return static_cast<const contracts::QolUiComponents*>(value);
}

void clicked(void*) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return;
    const void* value = nullptr;
    if (!contracts::succeeded(sdk->capabilities.require(contracts::kUiPanelsCapabilityId,
            {contracts::kUiPanelsVersionMajor, contracts::kUiPanelsVersionMinor}, &value)) ||
        value == nullptr) return;
    const auto* panels = static_cast<const contracts::UiPanelsApi*>(value);
    if (panels->open != nullptr) (void)panels->open("item_editor");
}

QolResult layout(void* parent, contracts::UiRect* out, void*) {
    const auto* ui = host_sdk() == nullptr ? nullptr : host_sdk()->ui;
    if (parent == nullptr || out == nullptr || ui == nullptr || ui->control_absolute_origin == nullptr ||
        ui->screen_inset_width == nullptr || ui->screen_inset_height == nullptr)
        return contracts::error(kQolErrNotReady, "item_editor entry layout", 24);
    int64_t px = 0, py = 0;
    int32_t inset_w = 0, inset_h = 0;
    if (!contracts::succeeded(ui->control_absolute_origin(parent, &px, &py)) ||
        !contracts::succeeded(ui->screen_inset_width(&inset_w)) ||
        !contracts::succeeded(ui->screen_inset_height(&inset_h)))
        return contracts::error(kQolErrNotReady, "item_editor entry origin", 25);
    constexpr int64_t base_w = 0x3c0, base_h = 0x280, size = 68;
    const int64_t screen_w = base_w + static_cast<int64_t>(inset_w) * 2;
    const int64_t screen_h = base_h + static_cast<int64_t>(inset_h) * 2;
    const int64_t logical_w = screen_h > 0 ? screen_w * base_h / screen_h : base_w;
    const int64_t x = logical_w - contracts::ui_theme::kNavBoxAbsX - contracts::ui_theme::kNavBoxW +
        (contracts::ui_theme::kNavBoxW - size) / 2 - (size + 8);
    const int64_t y = contracts::ui_theme::kNavBoxAbsY + (contracts::ui_theme::kNavBoxH - size) / 2;
    *out = {x - px, y - py, size, size};
    return contracts::ok();
}

void draw_entry() {
    const auto* c = components();
    if (c == nullptr) {
        static bool warned = false;
        if (!warned) { warned = true; log_info("components unavailable"); }
        return;
    }
    if (!item_editor_enabled()) {
        static bool warned = false;
        if (!warned) { warned = true; log_info("entry hidden reason=disabled"); }
        if (c->anchored_button_set_visible != nullptr) (void)c->anchored_button_set_visible(&g_entry, 0);
        return;
    }
    if (host_sdk() == nullptr || host_sdk()->security == nullptr ||
        host_sdk()->security->op_enabled == nullptr) return;
    uint8_t op = 0;
    if (!contracts::succeeded(host_sdk()->security->op_enabled(host_sdk()->security->context, &op)) || op == 0) {
        if (c->anchored_button_set_visible != nullptr) (void)c->anchored_button_set_visible(&g_entry, 0);
        item_editor_force_disable();
        return;
    }
    if (c->anchored_button_ensure == nullptr || c->anchored_button_draw == nullptr) return;
    const contracts::UiIconButtonDesc button{{0, 0, 68, 68}, 0x16, 0x00, 1, 1, "编", &clicked, nullptr, 0};
    const contracts::UiAnchoredButtonDesc desc{
        static_cast<uint32_t>(contracts::QolUiRootKind::kEquipBagContainer), button};
    uint8_t created = 0;
    const QolResult ensure = c->anchored_button_ensure(&g_entry, &desc, &layout, nullptr, &created);
    if (!contracts::succeeded(ensure)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            char message[64]{};
            snprintf(message, sizeof(message), "entry ensure failed code=%d",
                          static_cast<int>(ensure.code));
            log_info(message);
        }
        return;
    }
    if (created != 0) log_info("entry installed");
    (void)c->anchored_button_set_visible(&g_entry, 1);
    (void)c->anchored_button_draw(&g_entry);
}
}

void item_editor_equip_draw_wrapper() {
    static bool logged = false;
    if (!logged) { logged = true; log_info("draw wrapper invoked"); }
    const auto* sdk = host_sdk();
    if (sdk != nullptr && sdk->ui != nullptr && sdk->ui->roll_dice_draw != nullptr)
        (void)sdk->ui->roll_dice_draw();
    draw_entry();
}
}
