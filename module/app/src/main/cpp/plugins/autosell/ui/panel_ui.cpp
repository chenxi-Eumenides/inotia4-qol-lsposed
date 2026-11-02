#include "panel_internal.h"

namespace qol::features::autosell {

const qol::contracts::QolUiComponents* g_ui_components = nullptr;

const qol::contracts::QolUiAdapter* ui_api() {
    const qol::contracts::QolHostSdk* sdk = host_sdk();
    return sdk == nullptr ? nullptr : sdk->ui;
}

bool ui_set_rect(void* ctrl, UiRect rect) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->set_rect != nullptr && ui->set_rect(ctrl, rect).ok == kQolOk;
}

bool ui_get_rect(void* ctrl, UiRect* rect) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->get_rect != nullptr && ui->get_rect(ctrl, rect).ok == kQolOk;
}

void* ui_create_root(UiRect rect) {
    const auto* ui = ui_api();
    if (ui == nullptr || ui->create_control == nullptr) return nullptr;
    void* ctrl = nullptr;
    if (ui->create_control(0, nullptr, &ctrl).ok != kQolOk || !ui_set_rect(ctrl, rect)) {
        return nullptr;
    }
    return ctrl;
}

bool ui_hit_test(void* ctrl, int64_t x, int64_t y, UiRect rect) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->hit_test != nullptr && ui->hit_test(ctrl, x, y, rect) != 0;
}

void ui_begin_frame() {
    const auto* ui = ui_api();
    if (ui != nullptr && ui->begin_frame != nullptr) (void)ui->begin_frame();
}

void ui_end_frame() {
    const auto* ui = ui_api();
    if (ui != nullptr && ui->end_frame != nullptr) (void)ui->end_frame();
}

void ui_fill_rect_alpha(UiRect rect, uint32_t color, uint32_t alpha) {
    const auto* ui = ui_api();
    if (ui != nullptr && ui->fill_rect_alpha != nullptr) {
        (void)ui->fill_rect_alpha(rect, color, alpha);
    }
}

bool ui_screen_inset_width(int32_t* out) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->screen_inset_width != nullptr &&
           ui->screen_inset_width(out).ok == kQolOk;
}

bool ui_screen_inset_height(int32_t* out) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->screen_inset_height != nullptr &&
           ui->screen_inset_height(out).ok == kQolOk;
}

bool ui_child_count(void* parent, uint32_t* out) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->child_count != nullptr && ui->child_count(parent, out).ok == kQolOk;
}

bool ui_child_at(void* parent, uint32_t index, void** out) {
    const auto* ui = ui_api();
    return ui != nullptr && ui->child_at != nullptr && ui->child_at(parent, index, out).ok == kQolOk;
}

void ui_uidesc_draw() {
    const auto* ui = ui_api();
    if (ui != nullptr && ui->uidesc_draw != nullptr) (void)ui->uidesc_draw();
}

// 面板打开只能经 core.custom_ui 发布的 `ui_panels` 能力：容器负责 popup 栈、PopupHost
// 回调与「组件回调内延迟到帧泵」保护。Host SDK 的 `panels.open` 只是注册表原始
// `provider->open()`，不建面板也不推进 popup 状态。
const qol::contracts::UiPanelsApi* autosell_panels_api() {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    if (sdk->capabilities.require(qol::contracts::kUiPanelsCapabilityId,
            {qol::contracts::kUiPanelsVersionMajor, qol::contracts::kUiPanelsVersionMinor},
            &iface).ok != kQolOk) return nullptr;
    return static_cast<const qol::contracts::UiPanelsApi*>(iface);
}

const qol::contracts::QolUiComponents* autosell_components() {
    if (g_ui_components != nullptr) return g_ui_components;
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    if (sdk->capabilities.require(qol::contracts::kUiComponentsCapabilityId,
            {qol::contracts::kUiContractMajor, qol::contracts::kUiContractMinor},
            &iface).ok != kQolOk) return nullptr;
    g_ui_components = static_cast<const qol::contracts::QolUiComponents*>(iface);
    return g_ui_components;
}

void autosell_request_panel_close() {
    const auto* sdk = host_sdk();
    if (sdk != nullptr && sdk->panels.close != nullptr) sdk->panels.close("autosell");
}

}  // namespace qol::features::autosell
