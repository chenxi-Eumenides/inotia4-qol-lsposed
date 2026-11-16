#include "../plugin.h"

#include "../../../contracts/ui.h"
#include "../../../contracts/ui_theme.h"
#include "../../../contracts/panels.h"
#include "../../../contracts/log_domain.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace qol::features::item_editor {
namespace {
contracts::QolUiAnchoredButton g_entry{};
// 连续 ensure 失败帧数：只用于把「入口画不出来」从 debug 日志提升成一条默认可见的 W。
uint32_t g_ensure_fail_frames = 0;
constexpr uint32_t kEnsureFailWarnFrames = 30;

void entry_log(int32_t level, int line, const char* format, ...) {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0) return;
    char source[64]{};
    std::snprintf(source, sizeof(source), "item_editor/entry.cpp:%d", line);
    const uint32_t message_len = static_cast<uint32_t>(length) < contracts::kMessageCap
                                     ? static_cast<uint32_t>(length)
                                     : contracts::kMessageCap;
    (void)sdk->log.write(level, static_cast<int32_t>(QolDomain::kUi), source, message, message_len);
}

bool debug_enabled() {
    const auto* sdk = host_sdk();
    return sdk != nullptr && sdk->log.debug_enabled != nullptr && sdk->log.debug_enabled() != 0;
}

#define ENTRY_LOG_INFO(...) entry_log(1, __LINE__, __VA_ARGS__)
#define ENTRY_LOG_WARN(...) entry_log(2, __LINE__, __VA_ARGS__)
#define ENTRY_LOG_DEBUG(...)        \
    do {                            \
        if (debug_enabled())        \
            entry_log(0, __LINE__, __VA_ARGS__); \
    } while (0)

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
        if (!warned) { warned = true; ENTRY_LOG_WARN("reject entry capability=ui_components reason=missing"); }
        return;
    }
    if (!item_editor_enabled()) {
        static bool warned = false;
        if (!warned) { warned = true; ENTRY_LOG_INFO("drop entry root_kind=equip_bag reason=disabled"); }
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
        // NotReady 在装备页刚打开时是正常态（容器/布局未就位），逐次记录只放在 debug。
        // 连续失败到阈值说明这一局入口已经画不出来，必须按 W 留一条默认可见的记录，
        // 否则问题只存在于 debug 日志里。
        if (ensure.code == kQolErrNotReady) {
            if (g_ensure_fail_frames < kEnsureFailWarnFrames) ++g_ensure_fail_frames;
            ENTRY_LOG_DEBUG("fail ensure entry root_kind=equip_bag code=3 reason=%s",
                            ensure.message != nullptr ? ensure.message : "unknown");
            if (g_ensure_fail_frames == kEnsureFailWarnFrames) {
                ENTRY_LOG_WARN("fail entry render root_kind=equip_bag code=3 reason=%s frames=%d",
                               ensure.message != nullptr ? ensure.message : "unknown",
                               static_cast<int>(kEnsureFailWarnFrames));
            }
        } else {
            ENTRY_LOG_WARN("fail ensure entry root_kind=equip_bag code=%d reason=%s",
                           static_cast<int>(ensure.code),
                           ensure.message != nullptr ? ensure.message : "unknown");
        }
        return;
    }
    g_ensure_fail_frames = 0;
    if (created != 0) ENTRY_LOG_INFO("install entry root_kind=equip_bag result=created");
    (void)c->anchored_button_set_visible(&g_entry, 1);
    (void)c->anchored_button_draw(&g_entry);
}
}

void item_editor_equip_draw_wrapper() {
    static bool logged = false;
    if (!logged) { logged = true; ENTRY_LOG_INFO("bind entry hook=equip_draw result=ok"); }
    const auto* sdk = host_sdk();
    if (sdk != nullptr && sdk->ui != nullptr && sdk->ui->roll_dice_draw != nullptr)
        (void)sdk->ui->roll_dice_draw();
    draw_entry();
}
}
