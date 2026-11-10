#include "panel.h"

#include <cstdint>
#include <cstdio>
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

// 主体装饰框：左右各留 24 内边距；框内两列各 400 宽、列间距 24，正好铺满框内宽度。
constexpr int32_t kBodyX = 44;
constexpr int32_t kBodyY = 104;
constexpr int64_t kBodyW = 872;
constexpr int64_t kBodyH = 412;
constexpr UiRect kBody{kBodyX, kBodyY, kBodyW, kBodyH};
constexpr int32_t kColumnInset = 24;
constexpr int32_t kColumnLeft = kBodyX + kColumnInset;
constexpr int32_t kColumnW = 400;
constexpr int32_t kColumnGap = 24;
constexpr int32_t kRowTop = 16;
constexpr int32_t kRowHeight = 76;
constexpr int32_t kRowsPerColumn = 5;
constexpr uint32_t kRowsPerPage = 10;

// 列内布局：名称靠左，数值居中于左右箭头之间，箭头与数值同一行。
constexpr int32_t kNameOffsetX = 16;
constexpr int32_t kRowTextOffsetY = 24;
constexpr int32_t kValueOffsetX = 286;  // 左右箭头盒中心的对称中点
constexpr int32_t kArrowW = 44;
constexpr int32_t kArrowH = 44;
constexpr int32_t kArrowOffsetY = 16;
constexpr int32_t kLeftArrowOffsetX = 184;
constexpr int32_t kRightArrowOffsetX = 344;
constexpr int32_t kDividerInsetX = 8;
constexpr int32_t kDividerH = 2;

// 框下方翻页区：两个箭头按钮以面板中线左右各偏 120，页码夹在中间，状态提示更靠下。
constexpr int32_t kPageButtonSize = 56;
constexpr int32_t kPageButtonY = 540;
constexpr int32_t kPageButtonShift = 120;
constexpr int32_t kPageLabelY = 556;
constexpr int32_t kStatusY = 604;

constexpr uint32_t kRowCount = static_cast<uint32_t>(RateId::kCount);
constexpr uint32_t kPageCount = (kRowCount + kRowsPerPage - 1) / kRowsPerPage;
static_assert(kRowsPerColumn * 2 == static_cast<int32_t>(kRowsPerPage), "page is two columns");
static_assert(kPageCount >= 1, "panel needs at least one page");

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolUiComponents* g_components = nullptr;
const contracts::QolUiAdapter* g_ui = nullptr;

bool g_active = false;
QolUiHandle g_root = nullptr;
QolUiHandle g_back = nullptr;
QolUiHandle g_page_buttons[2] = {};
bool g_page_button_enabled[2] = {};
QolUiHandle g_arrows[kRowCount][2] = {};
bool g_arrow_enabled[kRowCount][2] = {};
uint32_t g_page = 0;
bool g_page_dirty = false;
bool g_back_pressed = false;
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

struct PageContext {
    int32_t delta;
    uint64_t generation;
};

ArrowContext g_contexts[kRowCount][2] = {};
PageContext g_page_contexts[2] = {};

void arrow_clicked(void* user);
void page_clicked(void* user);

bool main_thread() {
    return g_ui != nullptr && g_ui->is_main_thread != nullptr && g_ui->is_main_thread() != 0;
}

bool components_ready() {
    return g_components != nullptr && g_ui != nullptr &&
        g_components->draw_control_text != nullptr &&
        g_components->draw_container_styled != nullptr &&
        g_components->dispatch_native_event != nullptr &&
        g_components->panel_create != nullptr &&
        g_components->arrow_button_create != nullptr &&
        g_components->arrow_button_draw != nullptr &&
        g_components->arrow_button_set_enabled != nullptr &&
        g_components->nav_button_create != nullptr &&
        g_components->nav_button_draw != nullptr &&
        g_components->destroy != nullptr &&
        g_ui->delete_control != nullptr &&
        g_ui->control_absolute_origin != nullptr && g_ui->fill_rect != nullptr &&
        g_ui->begin_frame != nullptr && g_ui->end_frame != nullptr &&
        g_ui->touch_coordinates != nullptr && g_ui->hit_test != nullptr;
}

bool op_extended() {
    const auto* security = g_sdk == nullptr ? nullptr : g_sdk->security;
    uint8_t enabled = 0;
    return security != nullptr && security->op_enabled != nullptr &&
        contracts::succeeded(security->op_enabled(security->context, &enabled)) && enabled != 0;
}

void set_message(const char* text, bool error) {
    g_message = text == nullptr ? "" : text;
    g_message_error = error;
}

void destroy_panel() {
    g_active = false;
    g_page_dirty = false;
    g_pending_row = -1;
    g_request_id = 0;
    g_back_pressed = false;
    ++g_generation;
    QolUiHandle root = g_root;
    g_root = nullptr;
    g_back = nullptr;
    g_page = 0;
    // 根是组件面板：destroy 递归注销并删除子按钮，随后再释放根原生控件。
    if (g_components != nullptr && root != nullptr) (void)g_components->destroy(root);
    if (g_ui != nullptr && root != nullptr && g_ui->delete_control != nullptr)
        (void)g_ui->delete_control(root);
    for (uint32_t row = 0; row < kRowCount; ++row) {
        for (int side = 0; side < 2; ++side) {
            g_arrows[row][side] = nullptr;
            g_arrow_enabled[row][side] = false;
        }
    }
    for (int i = 0; i < 2; ++i) {
        g_page_buttons[i] = nullptr;
        g_page_button_enabled[i] = false;
    }
    g_message.clear();
    g_message_error = false;
    std::memset(g_contexts, 0, sizeof(g_contexts));
    std::memset(g_page_contexts, 0, sizeof(g_page_contexts));
}

// 列内第 line 行的控件纵坐标（root 局部）。
int32_t row_local_y(uint32_t line) {
    return kBodyY + kRowTop + static_cast<int32_t>(line) * kRowHeight;
}

// 页内第 slot 项所在列的横坐标（root 局部）。
int32_t column_local_x(uint32_t slot) {
    return kColumnLeft + static_cast<int32_t>(slot / kRowsPerColumn) * (kColumnW + kColumnGap);
}

uint32_t page_first_row() { return g_page * kRowsPerPage; }

uint32_t page_end_row() {
    const uint32_t first = page_first_row();
    const uint32_t end = first + kRowsPerPage;
    return end < kRowCount ? end : kRowCount;
}

void draw_row(uint32_t index, int32_t column_x, int32_t row_y) {
    if (index >= kRowCount || g_components == nullptr || g_root == nullptr) return;
    const auto id = static_cast<RateId>(index);
    const char* name = rate_name(id);
    if (name != nullptr) {
        (void)g_components->draw_control_text(g_root, name, column_x + kNameOffsetX,
            row_y + kRowTextOffsetY, contracts::ui_theme::kTextGold, 0, 1);
    }
    const std::string value = rate_display(id, g_rates.value[index]);
    (void)g_components->draw_control_text(g_root, value.c_str(), column_x + kValueOffsetX,
        row_y + kRowTextOffsetY, contracts::ui_theme::kTextNormal, 2, 1);
}

// 行间分隔线用绝对坐标直绘；行本身不画底色，只保留金色横线。
void draw_divider(int64_t root_x, int64_t root_y, int32_t column_x, int32_t row_y) {
    if (g_ui == nullptr || g_ui->fill_rect == nullptr) return;
    (void)g_ui->fill_rect({root_x + column_x + kDividerInsetX,
                           root_y + row_y + kRowHeight - kDividerH,
                           kColumnW - kDividerInsetX * 2, kDividerH},
                          contracts::ui_theme::kFrameGold);
}

void sync_arrow_states() {
    if (g_components == nullptr || g_components->arrow_button_set_enabled == nullptr) return;
    const bool config_ready = rates_config_available();
    const bool extended = op_extended();
    for (uint32_t row = 0; row < kRowCount; ++row) {
        const auto id = static_cast<RateId>(row);
        // 只禁用正在保存的行；其它行保持原状态，避免整屏箭头闪动。
        const bool pending = g_pending_row == static_cast<int>(row);
        for (int side = 0; side < 2; ++side) {
            QolUiHandle handle = g_arrows[row][side];
            if (handle == nullptr) continue;
            const bool can_shift = ladder_can_shift(id, g_confirmed.value[row], side == 0 ? -1 : 1,
                                                    extended);
            const bool enabled = config_ready && !pending && can_shift;
            if (g_arrow_enabled[row][side] == enabled) continue;
            g_arrow_enabled[row][side] = enabled;
            (void)g_components->arrow_button_set_enabled(handle, enabled ? 1 : 0);
        }
    }
}

void sync_page_buttons() {
    if (g_components == nullptr || g_components->arrow_button_set_enabled == nullptr) return;
    const bool states[2] = {g_page > 0, static_cast<uint64_t>(g_page) + 1 < kPageCount};
    for (int i = 0; i < 2; ++i) {
        if (g_page_buttons[i] == nullptr || g_page_button_enabled[i] == states[i]) continue;
        g_page_button_enabled[i] = states[i];
        (void)g_components->arrow_button_set_enabled(g_page_buttons[i], states[i] ? 1 : 0);
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
        context->row >= kRowCount || context->row / kRowsPerPage != g_page ||
        g_pending_row >= 0) {
        return;
    }
    const uint32_t row = context->row;
    if (!rates_config_available()) return;
    const int32_t current = g_confirmed.value[row];
    const bool extended = op_extended();
    const int32_t next = ladder_shift(static_cast<RateId>(row), current, context->delta, extended);
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

// 翻页回调只改页码与重建标记；销毁/重建箭头留到 panel_process 的安全点。
void page_clicked(void* user) {
    const auto* context = static_cast<const PageContext*>(user);
    if (!g_active || context == nullptr || context->generation != g_generation ||
        g_page_dirty) {
        return;
    }
    const int64_t target = static_cast<int64_t>(g_page) + context->delta;
    if (target < 0 || target >= static_cast<int64_t>(kPageCount)) return;
    g_page = static_cast<uint32_t>(target);
    g_page_dirty = true;
}

bool create_arrow(uint32_t row, int32_t delta) {
    const uint32_t side = delta < 0 ? 0u : 1u;
    const uint32_t slot = row - page_first_row();
    const int32_t column_x = column_local_x(slot);
    const int32_t row_y = row_local_y(slot % kRowsPerColumn);
    g_contexts[row][side] = {row, delta, g_generation};
    const contracts::UiArrowButtonDesc desc{
        {column_x + (delta < 0 ? kLeftArrowOffsetX : kRightArrowOffsetX),
         row_y + kArrowOffsetY, kArrowW, kArrowH},
        delta < 0 ? contracts::kUiArrowLeft : contracts::kUiArrowRight,
        1, &arrow_clicked, &g_contexts[row][side]};
    const bool created = contracts::succeeded(
        g_components->arrow_button_create(&desc, g_root, &g_arrows[row][side]));
    g_arrow_enabled[row][side] = created;
    return created;
}

// 释放上一页的箭头后按当前页重建；只创建本页实际存在的行。
bool rebuild_page_arrows() {
    if (g_components == nullptr || g_root == nullptr) return false;
    for (uint32_t row = 0; row < kRowCount; ++row) {
        for (int side = 0; side < 2; ++side) {
            if (g_arrows[row][side] != nullptr) (void)g_components->destroy(g_arrows[row][side]);
            g_arrows[row][side] = nullptr;
            g_arrow_enabled[row][side] = false;
        }
    }
    const uint32_t end = page_end_row();
    for (uint32_t row = page_first_row(); row < end; ++row) {
        if (!create_arrow(row, -1) || !create_arrow(row, 1)) return false;
    }
    return true;
}

void build_panel() {
    int32_t inset_x = 0, inset_y = 0;
    if (g_ui->screen_inset_width != nullptr) (void)g_ui->screen_inset_width(&inset_x);
    if (g_ui->screen_inset_height != nullptr) (void)g_ui->screen_inset_height(&inset_y);
    const contracts::UiPanelDesc panel{{inset_x, inset_y, kPanelW, kPanelH},
        {inset_x, inset_y, kPanelW, kPanelH}, 0, 0};
    if (!contracts::succeeded(g_components->panel_create(&panel, nullptr, &g_root)) ||
        g_root == nullptr) {
        g_root = nullptr;
        return;
    }
    const contracts::UiNavButtonDesc nav{
        {contracts::ui_theme::kNavBoxAbsX - inset_x,
         contracts::ui_theme::kNavBoxAbsY - inset_y,
         contracts::ui_theme::kNavBoxW, contracts::ui_theme::kNavBoxH},
        contracts::kUiNavButtonAuto, nullptr, nullptr};
    if (!contracts::succeeded(g_components->nav_button_create(&nav, g_root, &g_back)) ||
        g_back == nullptr) {
        destroy_panel();
        return;
    }
    if (g_ui->load_image_unit != nullptr) {
        (void)g_ui->load_image_unit(0x0f);
    }
    const int32_t center_x = static_cast<int32_t>(kPanelW / 2);
    for (int i = 0; i < 2; ++i) {
        g_page_contexts[i] = {i == 0 ? -1 : 1, g_generation};
        const bool enabled = i == 1 && kPageCount > 1;
        const contracts::UiArrowButtonDesc desc{
            {center_x + (i == 0 ? -kPageButtonShift : kPageButtonShift) - kPageButtonSize / 2,
             kPageButtonY, kPageButtonSize, kPageButtonSize},
            i == 0 ? contracts::kUiArrowLeft : contracts::kUiArrowRight,
            static_cast<uint8_t>(enabled ? 1 : 0), &page_clicked, &g_page_contexts[i]};
        if (!contracts::succeeded(g_components->arrow_button_create(&desc, g_root,
                &g_page_buttons[i])) || g_page_buttons[i] == nullptr) {
            destroy_panel();
            return;
        }
        g_page_button_enabled[i] = enabled;
    }
    if (!rebuild_page_arrows()) {
        destroy_panel();
        return;
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
    g_page = 0;
    g_page_dirty = false;
    set_message("", false);
    build_panel();
    if (g_root == nullptr) return;
    g_active = true;
}

void panel_process() {
    if (!main_thread() || !g_active || g_root == nullptr) return;
    poll_request();
    if (g_page_dirty) {
        g_page_dirty = false;
        if (!rebuild_page_arrows()) {
            destroy_panel();
            return;
        }
    }
    sync_arrow_states();
    sync_page_buttons();
    if (!contracts::succeeded(g_ui->begin_frame())) return;
    int64_t root_x = 0, root_y = 0;
    const bool have_origin =
        contracts::succeeded(g_ui->control_absolute_origin(g_root, &root_x, &root_y));
    if (have_origin) {
        const UiRect body{root_x + kBody.x, root_y + kBody.y, kBody.w, kBody.h};
        const contracts::UiContainerStyleDesc body_style{
            body, contracts::ui_theme::kPanelBgRgb565, contracts::ui_theme::kPanelAlpha, 1};
        (void)g_components->draw_container_styled(&body_style);
    }
    const uint32_t first = page_first_row();
    const uint32_t end = page_end_row();
    for (uint32_t row = first; row < end; ++row) {
        const uint32_t slot = row - first;
        const int32_t column_x = column_local_x(slot);
        const int32_t row_y = row_local_y(slot % kRowsPerColumn);
        if (have_origin) draw_divider(root_x, root_y, column_x, row_y);
        draw_row(row, column_x, row_y);
        for (auto& handle : g_arrows[row]) {
            if (handle != nullptr) (void)g_components->arrow_button_draw(handle);
        }
    }
    char page_label[32]{};
    std::snprintf(page_label, sizeof(page_label), "第 %u / %u 页", g_page + 1, kPageCount);
    (void)g_components->draw_control_text(g_root, page_label, 480, kPageLabelY,
        kPageCount > 1 ? contracts::ui_theme::kTextNormal : contracts::ui_theme::kTextDisabled, 2, 1);
    for (auto handle : g_page_buttons) {
        if (handle != nullptr) (void)g_components->arrow_button_draw(handle);
    }
    if (!g_message.empty()) {
        (void)g_components->draw_control_text(g_root, g_message.c_str(), 480, kStatusY,
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
        (void)g_components->dispatch_native_event(g_root, event, param, param2, &consumed);
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
    uint32_t consumed = 0;
    (void)g_components->dispatch_native_event(g_root, event, param, param2, &consumed);
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
