#include "panel.h"

#include <cstdint>
#include <cstring>
#include <string>

#include "../../contracts/host_sdk.h"
#include "../../contracts/ui_theme.h"
#include "feature.h"
#include "rates.h"

namespace qol::features::world_tuning {
namespace {

using contracts::QolUiHandle;
using contracts::UiRect;

constexpr const char* kPanelId = "world_tuning";
constexpr int64_t kPanelW = 960;
constexpr int64_t kPanelH = 640;
constexpr UiRect kBody{28, 100, 920, 444};
constexpr UiRect kList{36, 108, 888, 428};
constexpr int32_t kRowHeight = 71;
constexpr int32_t kArrowW = 44;
constexpr int32_t kArrowH = 44;
constexpr int32_t kArrowTop = 13;
constexpr int32_t kLeftArrowX = 664;
constexpr int32_t kRightArrowX = 824;
constexpr int32_t kValueCenterX = 766;
constexpr uint32_t kRowCount = static_cast<uint32_t>(RateId::kCount);

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolUiComponents* g_components = nullptr;
const contracts::QolUiAdapter* g_ui = nullptr;

bool g_active = false;
QolUiHandle g_root = nullptr;
QolUiHandle g_list = nullptr;
QolUiHandle g_back = nullptr;
QolUiHandle g_rows[kRowCount] = {};
QolUiHandle g_pressed_list = nullptr;
bool g_back_pressed = false;
QolUiHandle g_arrows[kRowCount][2] = {};
bool g_arrow_enabled[kRowCount][2] = {};
Rates g_rates{};
Rates g_confirmed{};
int g_pending_row = -1;
uint64_t g_request_id = 0;
uint64_t g_generation = 0;
std::string g_message;
bool g_message_error = false;

struct ArrowContext {
    uint32_t row;
    int32_t delta;
    uint64_t generation;
};

ArrowContext g_contexts[kRowCount][2] = {};

void arrow_clicked(void* user);

bool main_thread() {
    return g_ui != nullptr && g_ui->is_main_thread != nullptr && g_ui->is_main_thread() != 0;
}

bool components_ready() {
    return g_components != nullptr && g_ui != nullptr &&
        g_components->draw_control_text != nullptr &&
        g_components->draw_container_styled != nullptr &&
        g_components->dispatch_native_event != nullptr &&
        g_components->arrow_button_create != nullptr &&
        g_components->arrow_button_draw != nullptr &&
        g_components->arrow_button_set_enabled != nullptr &&
        g_components->nav_button_create != nullptr &&
        g_components->nav_button_draw != nullptr &&
        g_components->scroll_list_create != nullptr &&
        g_components->scroll_list_process != nullptr &&
        g_components->scroll_list_draw != nullptr &&
        g_components->scroll_list_get_row != nullptr &&
        g_components->scroll_list_set_selection_enabled != nullptr &&
        g_components->destroy != nullptr && g_ui->create_control != nullptr &&
        g_ui->set_rect != nullptr && g_ui->delete_control != nullptr &&
        g_ui->control_absolute_origin != nullptr && g_ui->fill_rect != nullptr &&
        g_ui->begin_frame != nullptr && g_ui->end_frame != nullptr &&
        g_ui->touch_coordinates != nullptr && g_ui->hit_test != nullptr;
}

void set_message(const char* text, bool error) {
    g_message = text == nullptr ? "" : text;
    g_message_error = error;
}

void destroy_panel() {
    g_active = false;
    g_pending_row = -1;
    g_request_id = 0;
    g_pressed_list = nullptr;
    g_back_pressed = false;
    ++g_generation;
    for (uint32_t row = 0; row < kRowCount; ++row) {
        for (int side = 0; side < 2; ++side) {
            g_arrows[row][side] = nullptr;
            g_arrow_enabled[row][side] = false;
        }
    }
    for (auto& handle : g_rows) handle = nullptr;
    if (g_components != nullptr) {
        if (g_list != nullptr) (void)g_components->destroy(g_list);
        if (g_back != nullptr) (void)g_components->destroy(g_back);
    }
    g_list = nullptr;
    g_back = nullptr;
    if (g_ui != nullptr && g_root != nullptr && g_ui->delete_control != nullptr)
        (void)g_ui->delete_control(g_root);
    g_root = nullptr;
    g_message.clear();
    g_message_error = false;
    std::memset(g_contexts, 0, sizeof(g_contexts));
}

void draw_row(QolUiHandle row, uint32_t index) {
    if (row == nullptr || index >= kRowCount || g_components == nullptr) return;
    const auto id = static_cast<RateId>(index);
    const char* name = rate_name(id);
    if (name != nullptr) {
        (void)g_components->draw_control_text(row, name, 16, 22, contracts::ui_theme::kTextGold, 0, 1);
    }
    const std::string value = rate_display(g_rates.value[index]);
    (void)g_components->draw_control_text(row, value.c_str(), kValueCenterX, 24,
        contracts::ui_theme::kTextNormal, 2, 1);
}

void draw_item(void*, uint32_t index, QolUiHandle row, uint8_t) {
    if (g_ui == nullptr || g_ui->control_absolute_origin == nullptr ||
        g_ui->fill_rect == nullptr) {
        return;
    }
    int64_t x = 0, y = 0;
    if (contracts::succeeded(g_ui->control_absolute_origin(row, &x, &y))) {
        (void)g_ui->fill_rect({x, y + kRowHeight - 2, kList.w, 2},
                              contracts::ui_theme::kFrameGold);
    }
    draw_row(row, index);
    for (auto& handle : g_arrows[index]) {
        if (handle != nullptr) (void)g_components->arrow_button_draw(handle);
    }
}

void sync_arrow_states() {
    if (g_components == nullptr || g_components->arrow_button_set_enabled == nullptr) return;
    const bool config_ready = rates_config_available();
    for (uint32_t row = 0; row < kRowCount; ++row) {
        const int32_t index = ladder_index(g_confirmed.value[row]);
        // 只禁用正在保存的行；其它行保持原状态，避免整屏箭头闪动。
        const bool pending = g_pending_row == static_cast<int>(row);
        for (int side = 0; side < 2; ++side) {
            QolUiHandle handle = g_arrows[row][side];
            if (handle == nullptr) continue;
            const bool at_edge = index < 0 ||
                (side == 0 ? index == 0 : index == static_cast<int32_t>(kRateLadderSize) - 1);
            const bool enabled = config_ready && !pending && !at_edge;
            if (g_arrow_enabled[row][side] == enabled) continue;
            g_arrow_enabled[row][side] = enabled;
            (void)g_components->arrow_button_set_enabled(handle, enabled ? 1 : 0);
        }
    }
}

void poll_request() {
    if (g_request_id == 0) return;
    contracts::FeatureReceipt receipt{};
    if (!poll_rates_receipt(g_request_id, &receipt)) {
        g_request_id = 0;
        g_pending_row = -1;
        g_rates = g_confirmed;
        set_message("保存状态未知，请重新打开面板核对", true);
        return;
    }
    if (receipt.persistence == contracts::FeaturePersistence::kPending) return;
    const bool persisted = receipt.persistence == contracts::FeaturePersistence::kPersisted &&
        receipt.admission == contracts::FeatureAdmission::kAdmitted;
    g_request_id = 0;
    g_pending_row = -1;
    if (!persisted) {
        g_rates = g_confirmed;
        set_message("保存失败，数值未更改", true);
        return;
    }
    g_confirmed = g_rates;
    apply_rates(g_confirmed);
    set_message("", false);
}

void arrow_clicked(void* user) {
    const auto* context = static_cast<const ArrowContext*>(user);
    if (!g_active || context == nullptr || context->generation != g_generation ||
        context->row >= kRowCount || g_pending_row >= 0) {
        return;
    }
    const uint32_t row = context->row;
    if (!rates_config_available()) return;
    const int32_t current = g_confirmed.value[row];
    const int32_t next = ladder_shift(current, context->delta);
    if (next == current) return;
    Rates candidate = g_confirmed;
    candidate.value[row] = next;
    uint64_t request_id = 0;
    if (!request_persist_rates(candidate, &request_id)) {
        set_message("保存失败，数值未更改", true);
        return;
    }
    g_pending_row = static_cast<int>(row);
    g_request_id = request_id;
    g_rates = candidate;
}

bool create_arrow(uint32_t row, int32_t delta) {
    const uint32_t side = delta < 0 ? 0u : 1u;
    g_contexts[row][side] = {row, delta, g_generation};
    const contracts::UiArrowButtonDesc desc{
        {delta < 0 ? kLeftArrowX : kRightArrowX, kArrowTop, kArrowW, kArrowH},
        delta < 0 ? contracts::kUiArrowLeft : contracts::kUiArrowRight,
        1, &arrow_clicked, &g_contexts[row][side]};
    const bool created = contracts::succeeded(
        g_components->arrow_button_create(&desc, g_rows[row], &g_arrows[row][side]));
    g_arrow_enabled[row][side] = created;
    return created;
}

void build_panel() {
    int32_t inset_x = 0, inset_y = 0;
    if (g_ui->screen_inset_width != nullptr) (void)g_ui->screen_inset_width(&inset_x);
    if (g_ui->screen_inset_height != nullptr) (void)g_ui->screen_inset_height(&inset_y);
    if (!contracts::succeeded(g_ui->create_control(0, nullptr, &g_root)) || g_root == nullptr) {
        g_root = nullptr;
        return;
    }
    (void)g_ui->set_rect(g_root, {inset_x, inset_y, kPanelW, kPanelH});
    const contracts::UiNavButtonDesc nav{
        {contracts::ui_theme::kNavBoxAbsX - inset_x,
         contracts::ui_theme::kNavBoxAbsY - inset_y,
         contracts::ui_theme::kNavBoxW, contracts::ui_theme::kNavBoxH},
        contracts::kUiNavButtonAuto, nullptr, nullptr};
    (void)g_components->nav_button_create(&nav, g_root, &g_back);
    if (g_ui->load_image_unit != nullptr) {
        (void)g_ui->load_image_unit(0x0f);
        (void)g_ui->load_image_unit(0x4f);
        (void)g_ui->load_image_unit(0x50);
    }
    const contracts::UiScrollListDesc list{kList, kRowHeight, kRowCount, -1, &draw_item, nullptr,
                                           nullptr};
    if (!contracts::succeeded(g_components->scroll_list_create(&list, g_root, &g_list))) {
        destroy_panel();
        return;
    }
    (void)g_components->scroll_list_set_selection_enabled(g_list, 0);
    for (uint32_t row = 0; row < kRowCount; ++row) {
        if (!contracts::succeeded(g_components->scroll_list_get_row(g_list, row, &g_rows[row])) ||
            g_rows[row] == nullptr) {
            destroy_panel();
            return;
        }
    }
    for (uint32_t row = 0; row < kRowCount; ++row) {
        if (!create_arrow(row, -1) || !create_arrow(row, 1)) {
            destroy_panel();
            return;
        }
    }
}

void panel_enter() {
    if (!main_thread() || !components_ready()) return;
    if (g_root != nullptr) destroy_panel();
    reload_rates_from_config();
    g_confirmed = current_rates();
    g_rates = g_confirmed;
    g_pending_row = -1;
    g_request_id = 0;
    set_message("", false);
    build_panel();
    if (g_root == nullptr) return;
    g_active = true;
}

void panel_process() {
    if (!main_thread() || !g_active || g_root == nullptr) return;
    poll_request();
    sync_arrow_states();
    if (g_list != nullptr) (void)g_components->scroll_list_process(g_list);
    if (!contracts::succeeded(g_ui->begin_frame())) return;
    int64_t root_x = 0, root_y = 0;
    if (contracts::succeeded(g_ui->control_absolute_origin(g_root, &root_x, &root_y))) {
        if (g_ui->draw_image_part != nullptr) {
            (void)g_ui->draw_image_part(0x4f, 0, root_x + kPanelW / 2 - 300,
                                        root_y + kPanelH / 2, 2, 1);
            (void)g_ui->draw_image_part(0x50, 0, root_x + kPanelW / 2 + 300,
                                        root_y + kPanelH / 2, 2, 1);
        }
        const UiRect body{root_x + kBody.x, root_y + kBody.y, kBody.w, kBody.h};
        const contracts::UiContainerStyleDesc body_style{body, 0x1904, 100, 1};
        (void)g_components->draw_container_styled(&body_style);
    }
    if (g_list != nullptr) (void)g_components->scroll_list_draw(g_list);
    if (!g_message.empty()) {
        (void)g_components->draw_control_text(g_root, g_message.c_str(), 480, 572,
            g_message_error ? contracts::ui_theme::kError : contracts::ui_theme::kOk, 2, 0);
    }
    if (g_back != nullptr) (void)g_components->nav_button_draw(g_back);
    (void)g_ui->end_frame();
}

void panel_f3() { if (main_thread()) destroy_panel(); }
void panel_f4() {}

uint32_t panel_event(uint64_t event, uint64_t param, uint64_t param2) {
    if (!main_thread() || !g_active || g_root == nullptr) return 1;
    if (event != 0x17 && event != 0x18 && event != 0x19) return 1;
    if (event == 0x19) {
        uint32_t consumed = 0;
        (void)g_components->dispatch_native_event(
            g_pressed_list != nullptr ? g_pressed_list : g_root, event, param, param2, &consumed);
        return consumed;
    }
    int64_t x = 0, y = 0;
    if (param == 0 || g_ui->touch_coordinates == nullptr ||
        !contracts::succeeded(g_ui->touch_coordinates(param, &x, &y))) {
        return 1;
    }
    if (g_back != nullptr) {
        const bool inside = g_ui->hit_test(g_back, x, y,
            {0, 0, contracts::ui_theme::kNavBoxW, contracts::ui_theme::kNavBoxH}) > 0;
        if ((event == 0x17 && inside) || (event == 0x18 && g_back_pressed)) {
            g_back_pressed = event == 0x17;
            uint32_t consumed = 0;
            (void)g_components->dispatch_native_event(g_root, event, param, param2, &consumed);
            return consumed;
        }
    }
    if (event == 0x17) {
        g_pressed_list = g_list != nullptr &&
            g_ui->hit_test(g_list, x, y, {0, 0, kList.w, kList.h}) > 0 ? g_list : nullptr;
    }
    if (g_pressed_list == nullptr) return 1;
    uint32_t consumed = 0;
    (void)g_components->dispatch_native_event(g_pressed_list, event, param, param2, &consumed);
    if (event == 0x18) g_pressed_list = nullptr;
    return consumed;
}

bool provider_open() {
    if (!gate_enabled()) return false;
    panel_enter();
    return g_active && g_root != nullptr;
}

void provider_close() { destroy_panel(); }

bool provider_active() { return g_active; }

const char* provider_block_reason() {
    if (!gate_enabled()) return "游戏世界调整未启用";
    if (!panel_available()) return "界面工具不可用";
    return nullptr;
}

const contracts::PanelCallbacks kCallbacks{
    &panel_enter, &panel_process, &panel_f3, &panel_f4, &panel_event};

const contracts::PanelProviderSpec kProvider{
    kPanelId, kPanelId, &provider_open, &provider_close, &provider_active,
    &provider_block_reason, &kCallbacks};

}  // namespace

void panel_bind(const contracts::QolHostSdk* sdk, const contracts::QolUiComponents* components,
                const contracts::QolUiAdapter* ui) {
    g_sdk = sdk;
    g_components = components;
    g_ui = ui;
}

void panel_unbind() {
    destroy_panel();
    g_sdk = nullptr;
    g_components = nullptr;
    g_ui = nullptr;
}

const contracts::PanelProviderSpec& panel_provider() { return kProvider; }

bool panel_available() { return g_sdk != nullptr && components_ready(); }

}  // namespace qol::features::world_tuning
