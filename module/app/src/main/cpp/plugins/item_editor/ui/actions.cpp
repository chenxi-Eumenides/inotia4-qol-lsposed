#include "actions.h"

#include "../../../contracts/inventory_action.h"
#include "../../../contracts/inventory_transfer.h"
#include "../../../contracts/item_payload.h"
#include "../../../contracts/ui_confirmation.h"
#include "../../../contracts/ui_theme.h"
#include "panel_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace qol::features::item_editor::panel_internal {
namespace {

using namespace contracts;

// 按钮几何：独立 root 原点 = (panel.x + panel.w + 20, panel.y + 150)，
// 按钮 72×72、纵向步距 84；root 不画底，只承载命中控件。
constexpr int32_t kActionGap = 20;
constexpr int32_t kActionFirstY = 150;
constexpr int32_t kActionButtonSize = 72;
constexpr int32_t kActionButtonStride = 84;
constexpr int32_t kActionButtonCount = 3;
constexpr int32_t kReplaceButtonIndex = 2;
constexpr int32_t kStatusGap = 8;
constexpr int32_t kActionRootHeight =
    kActionButtonSize + (kActionButtonCount - 1) * kActionButtonStride +
    (kActionButtonStride - kActionButtonSize) + kStatusGap + 16;

// 0 无 / 1 成功 / 2 失败 / 3 禁用提示。
constexpr int32_t kStatusNone = 0;
constexpr int32_t kStatusSuccess = 1;
constexpr int32_t kStatusFailed = 2;
constexpr int32_t kStatusHint = 3;

enum class PendingAction : uint8_t { kNone, kPlaceBag, kDropItem, kReplaceItem };

QolUiHandle g_action_root = nullptr;
QolUiHandle g_action_buttons[kActionButtonCount]{};
PendingAction g_pending_action = PendingAction::kNone;
uint64_t g_confirmation_token = 0;
bool g_confirmation_open = false;
int32_t g_status_kind = kStatusNone;
char g_status_text[64]{};

const char* const kButtonLabels[kActionButtonCount][4] = {
    {"放", "入", "背", "包"},
    {"生", "成", "掉", "落"},
    {"替", "换", "物", "品"},
};
const char* const kConfirmMessages[kActionButtonCount] = {
    "放入背包？", "生成掉落？", "替换物品？",
};
const char* const kActionNames[kActionButtonCount] = {
    "place_bag", "drop_item", "replace_item",
};

void action_confirmed_place(void*) {
    g_confirmation_open = false;
    g_pending_action = PendingAction::kPlaceBag;
}
void action_confirmed_drop(void*) {
    g_confirmation_open = false;
    g_pending_action = PendingAction::kDropItem;
}
void action_confirmed_replace(void*) {
    g_confirmation_open = false;
    g_pending_action = PendingAction::kReplaceItem;
}
void action_cancelled(void*) {
    g_confirmation_open = false;
    g_pending_action = PendingAction::kNone;
}
constexpr UiConfirmationCallback kConfirmCallbacks[kActionButtonCount] = {
    &action_confirmed_place, &action_confirmed_drop, &action_confirmed_replace,
};

void noop_draw(void*) {}

void set_status(int32_t kind, const char* text) {
    g_status_kind = kind;
    g_status_text[0] = '\0';
    if (text != nullptr) std::snprintf(g_status_text, sizeof(g_status_text), "%s", text);
}

void log_action(const char* text) { log_panel(text); }

bool button_visible(int32_t index) {
    return index != kReplaceButtonIndex || g_draft.source_valid;
}

bool button_enabled(int32_t index) {
    if (!action_draft_ready(g_draft)) return false;
    if (index == kReplaceButtonIndex && !g_draft.source_valid) return false;
    return true;
}

int32_t pending_index(PendingAction action) {
    switch (action) {
        case PendingAction::kPlaceBag: return 0;
        case PendingAction::kDropItem: return 1;
        case PendingAction::kReplaceItem: return 2;
        case PendingAction::kNone: break;
    }
    return -1;
}

// 能力查找每次调用现取，不缓存租约；不可用时动作禁用并报状态。
const InventoryActionApi* action_api() {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    if (!succeeded(sdk->capabilities.require(kInventoryActionCapabilityId,
            {kInventoryActionVersionMajor, kInventoryActionVersionMinor}, &iface)))
        return nullptr;
    const auto* api = static_cast<const InventoryActionApi*>(iface);
    return inventory_action_api_valid(api) && api->create != nullptr &&
                   api->place != nullptr && api->discard_handle != nullptr
               ? api
               : nullptr;
}

const ItemPayloadApi* payload_api() {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    if (!succeeded(sdk->capabilities.require(kItemPayloadCapabilityId,
            {kItemPayloadVersionMajor, kItemPayloadVersionMinor}, &iface)))
        return nullptr;
    const auto* api = static_cast<const ItemPayloadApi*>(iface);
    return item_payload_codec_api_valid(api) ? api : nullptr;
}

const QolInventoryTransferApi* transfer_api() {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    if (!succeeded(sdk->capabilities.require(kInventoryTransferCapabilityId,
            {kInventoryTransferVersion.major, kInventoryTransferVersion.minor}, &iface)))
        return nullptr;
    const auto* api = static_cast<const QolInventoryTransferApi*>(iface);
    return qol_inventory_transfer_api_valid(api) ? api : nullptr;
}

const UiConfirmationApi* confirmation_api() {
    const auto* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    if (!succeeded(sdk->capabilities.require(kUiConfirmationCapabilityId,
            {static_cast<uint16_t>(kUiConfirmationVersionMajor),
             static_cast<uint16_t>(kUiConfirmationVersionMinor)},
            &iface)))
        return nullptr;
    const auto* api = static_cast<const UiConfirmationApi*>(iface);
    return api != nullptr && api->open_text != nullptr && api->close != nullptr ? api : nullptr;
}

// 按 codec 口径构建 payload；失败时 fail_step 记录阶段供日志区分。
bool build_payload(QolItemPayloadBlob* out, const char** fail_step) {
    if (!action_draft_ready(g_draft)) {
        *fail_step = "draft";
        return false;
    }
    QolItemPayloadOption options[kItemPayloadMaxOptionCount];
    const uint32_t needed =
        action_payload_options(g_draft, options, kItemPayloadMaxOptionCount);
    if (needed > kItemPayloadMaxOptionCount) {
        *fail_step = "option_capacity";
        return false;
    }
    const auto* api = payload_api();
    if (api == nullptr) {
        *fail_step = "api";
        return false;
    }
    QolItemPayloadBuildSpec spec{};
    spec.struct_size = sizeof(spec);
    spec.header = action_payload_header(g_draft);
    // 非装备的堆叠数量可能走 999 扩展编码；纯 header 推导只写 b 段，
    // 这里用 Host 的 item_count 编码器保持 canonical 数量一致。
    if (g_draft.class_index != kItemClassGem &&
        !item_class_is_equipment(g_draft.class_index)) {
        const auto* game = game_host();
        if (game != nullptr && game->item_count.field_from_canonical != nullptr) {
            uint8_t extended = 0;
            if (game->item_count.read_extended_mode != nullptr)
                (void)game->item_count.read_extended_mode(&extended);
            uint32_t field = 0;
            if (succeeded(game->item_count.field_from_canonical(
                    0, static_cast<uint32_t>(std::max(g_draft.count, 0)),
                    extended != 0 ? 1 : 0, &field))) {
                spec.header.count_field = field;
            }
        }
    }
    spec.options = options;
    spec.option_count = needed;
    spec.flags = 0;
    if (!succeeded(api->build(&spec, out))) {
        *fail_step = "build";
        return false;
    }
    return true;
}

void create_detached(const char* action_name, QolItemCreateOutcome* out, bool* ok) {
    const auto* api = action_api();
    if (api == nullptr || api->create == nullptr) {
        set_status(kStatusFailed, "功能不可用");
        char line[128];
        std::snprintf(line, sizeof(line),
                      "op=action action=%s step=api result=failed reason=action_unavailable",
                      action_name);
        log_action(line);
        *ok = false;
        return;
    }
    QolItemCreateSpec spec{};
    spec.struct_size = sizeof(spec);
    spec.category = g_draft.item_id;
    spec.count = 1;
    spec.flags = kQolItemCreateFlagAllocateUid;
    QolItemPayloadBlob blob{};
    const char* fail_step = nullptr;
    if (!build_payload(&blob, &fail_step)) {
        set_status(kStatusFailed, fail_step != nullptr && std::strcmp(fail_step, "api") == 0
                                      ? "功能不可用"
                                      : "生成数据失败");
        char line[128];
        std::snprintf(line, sizeof(line),
                      "op=action action=%s step=payload_build result=failed reason=%s",
                      action_name, fail_step == nullptr ? "unknown" : fail_step);
        log_action(line);
        *ok = false;
        return;
    }
    spec.payload = blob.payload;
    spec.payload_size = blob.payload_size;
    *out = api->create(&spec);
    if (out->status != InventoryActionStatus::kOk) {
        set_status(kStatusFailed, "生成物品失败");
        char line[128];
        std::snprintf(line, sizeof(line), "op=action action=%s step=create result=failed status=%d",
                      action_name, static_cast<int>(out->status));
        log_action(line);
        *ok = false;
    }
}

void execute_place_bag() {
    QolItemCreateOutcome created{};
    bool ok = true;
    create_detached("place_bag", &created, &ok);
    if (!ok) return;
    const auto* api = action_api();
    if (api == nullptr || api->place == nullptr) {
        // create 成功后能力查找不应失败（require 无状态、单线程）；不可用时
        // detached 句柄无处释放，只记录日志。
        set_status(kStatusFailed, "放入背包失败");
        log_action("op=action action=place_bag step=place result=failed reason=api_unavailable");
        return;
    }
    QolItemPlaceTarget target{};
    target.struct_size = sizeof(target);
    target.ref = {kQolBagSpaceOriginal, -1};
    target.slot = -1;
    target.flags = kQolItemPlaceFlagMerge;
    const QolItemPlaceOutcome placed = api->place(created.item, &target);
    if (placed.status != InventoryActionStatus::kOk) {
        // place 失败不 discard：原生写槽失败时对象可能已由 Host 释放，再次
        // discard 有误释放风险。
        set_status(kStatusFailed, "放入背包失败");
        char line[128];
        std::snprintf(line, sizeof(line), "op=action action=place_bag step=place result=failed status=%d",
                      static_cast<int>(placed.status));
        log_action(line);
        return;
    }
    set_status(kStatusSuccess, "已放入背包");
}

void execute_drop_item() {
    QolItemCreateOutcome created{};
    bool ok = true;
    create_detached("drop_item", &created, &ok);
    if (!ok) return;
    const auto* api = action_api();
    const auto* port = item_port();
    if (api == nullptr || api->discard_handle == nullptr || port == nullptr ||
        !qol_item_port_drop_size_valid(port) || port->read.drop_item == nullptr) {
        if (api != nullptr && api->discard_handle != nullptr)
            (void)api->discard_handle(created.item, 1);
        set_status(kStatusFailed, "生成掉落失败");
        log_action("op=action action=drop_item step=port result=failed reason=drop_unavailable");
        return;
    }
    if (succeeded(port->read.drop_item(created.item))) {
        // 成功后句柄已失效，不再 discard。
        set_status(kStatusSuccess, "已生成掉落");
        return;
    }
    (void)api->discard_handle(created.item, 1);
    set_status(kStatusFailed, "生成掉落失败");
    log_action("op=action action=drop_item step=drop result=failed");
}

void execute_replace_item() {
    if (!g_draft.source_valid) {
        set_status(kStatusFailed, "替换物品失败");
        log_action("op=action action=replace_item step=source result=failed reason=source_invalid");
        return;
    }
    QolItemPayloadBlob blob{};
    const char* fail_step = nullptr;
    if (!build_payload(&blob, &fail_step)) {
        set_status(kStatusFailed, fail_step != nullptr && std::strcmp(fail_step, "api") == 0
                                      ? "功能不可用"
                                      : "生成数据失败");
        char line[128];
        std::snprintf(line, sizeof(line),
                      "op=action action=replace_item step=payload_build result=failed reason=%s",
                      fail_step == nullptr ? "unknown" : fail_step);
        log_action(line);
        return;
    }
    const auto* api = transfer_api();
    if (api == nullptr) {
        set_status(kStatusFailed, "功能不可用");
        log_action("op=action action=replace_item step=api result=failed reason=transfer_unavailable");
        return;
    }
    QolBagTransfer transfer{};
    transfer.category = g_draft.item_id;
    transfer.count = 1;
    transfer.payload_size = blob.payload_size;
    std::memcpy(transfer.payload, blob.payload, blob.payload_size);
    const QolResult result = api->replace_item(g_draft.source, &transfer, nullptr);
    if (!succeeded(result)) {
        set_status(kStatusFailed, "替换物品失败");
        char line[128];
        std::snprintf(line, sizeof(line),
                      "op=action action=replace_item step=replace result=failed code=%d",
                      result.code);
        log_action(line);
        return;
    }
    set_status(kStatusSuccess, "已替换物品");
}

void process_pending_action() {
    if (g_pending_action == PendingAction::kNone) return;
    const PendingAction action = g_pending_action;
    g_pending_action = PendingAction::kNone;
    const int32_t index = pending_index(action);
    if (index < 0 || !button_enabled(index)) {
        set_status(kStatusHint, action_draft_ready(g_draft) ? "无法执行该动作" : "未选择物品");
        return;
    }
    switch (action) {
        case PendingAction::kPlaceBag: execute_place_bag(); break;
        case PendingAction::kDropItem: execute_drop_item(); break;
        case PendingAction::kReplaceItem: execute_replace_item(); break;
        case PendingAction::kNone: break;
    }
}

void action_button_clicked(void* user) {
    const intptr_t raw = reinterpret_cast<intptr_t>(user);
    if (raw < 0 || raw >= kActionButtonCount) return;
    const int32_t index = static_cast<int32_t>(raw);
    // 确认框打开期间按钮不重复触发。
    if (g_confirmation_open) return;
    if (!button_enabled(index)) {
        set_status(kStatusHint, action_draft_ready(g_draft) ? "无法执行该动作" : "未选择物品");
        return;
    }
    const auto* api = confirmation_api();
    if (api == nullptr) {
        set_status(kStatusFailed, "确认框不可用");
        char line[128];
        std::snprintf(line, sizeof(line),
                      "op=action action=%s step=confirmation result=failed reason=api_unavailable",
                      kActionNames[index]);
        log_action(line);
        return;
    }
    UiConfirmationSpecText spec{};
    spec.size = sizeof(spec);
    std::snprintf(spec.message, sizeof(spec.message), "%s", kConfirmMessages[index]);
    spec.price = 0;
    spec.on_confirm = kConfirmCallbacks[index];
    spec.on_cancel = &action_cancelled;
    spec.user = nullptr;
    uint64_t token = 0;
    const QolResult result = api->open_text(&spec, &token);
    if (!succeeded(result) || token == 0) {
        // open_text 失败但返回非零 token 时按契约 close 该 token 释放 owner 状态。
        if (token != 0) (void)api->close(token);
        set_status(kStatusFailed, "确认框不可用");
        char line[128];
        std::snprintf(line, sizeof(line),
                      "op=action action=%s step=confirmation result=failed code=%d token=%llu",
                      kActionNames[index], result.code,
                      static_cast<unsigned long long>(token));
        log_action(line);
        return;
    }
    g_confirmation_token = token;
    g_confirmation_open = true;
}

void draw_action_status() {
    int32_t kind = g_status_kind;
    const char* text = g_status_text;
    if (kind == kStatusNone && !action_draft_ready(g_draft)) {
        kind = kStatusHint;
        text = "未选择物品";
    }
    if (kind == kStatusNone || text == nullptr || text[0] == '\0') return;
    uint32_t color = kMuted;
    if (kind == kStatusSuccess) color = ui_theme::kOk;
    if (kind == kStatusFailed) color = ui_theme::kError;
    const int32_t width = text_pixel_width(text, 0);
    // 状态文本画在按钮列下方，右缘对齐按钮左缘，避免向屏幕右外侧溢出。
    const int32_t offset_y = kActionButtonCount * kActionButtonStride -
                             (kActionButtonStride - kActionButtonSize) + kStatusGap;
    draw_text(g_action_root, text, -width, offset_y, color, 0, 0);
}

void draw_action_buttons() {
    const auto* adapter = ui();
    if (adapter == nullptr || g_action_root == nullptr) return;
    const UiRect panel = panel_rect();
    const int32_t bx = static_cast<int32_t>(panel.x + panel.w) + kActionGap;
    const int32_t root_y = static_cast<int32_t>(panel.y) + kActionFirstY;
    for (int32_t index = 0; index < kActionButtonCount; ++index) {
        if (!button_visible(index)) continue;
        const bool enabled = button_enabled(index);
        const int32_t by = root_y + index * kActionButtonStride;
        int32_t pressed = 0;
        if (enabled && g_action_buttons[index] != nullptr && adapter->control_pressed != nullptr)
            (void)adapter->control_pressed(g_action_buttons[index], &pressed);
        if (adapter->draw_help_option_row != nullptr)
            (void)adapter->draw_help_option_row({bx, by, kActionButtonSize, kActionButtonSize}, 0,
                                                static_cast<uint8_t>(pressed != 0));
        const uint32_t color = enabled ? kGold : kMuted;
        for (int32_t quad = 0; quad < 4; ++quad) {
            const char* glyph = kButtonLabels[index][quad];
            const int32_t width = text_pixel_width(glyph, 0);
            const int32_t center_x = kActionButtonSize / 4 + (quad & 1) * (kActionButtonSize / 2);
            const int32_t center_y = kActionButtonSize / 4 + (quad >> 1) * (kActionButtonSize / 2);
            // draw_control_text 的偏移相对 action root 原点；y 以字形上缘对齐，
            // 用字宽近似方框字高做象限居中。
            const int32_t offset_x = center_x - width / 2;
            const int32_t offset_y =
                index * kActionButtonStride + center_y - width / 2;
            draw_text(g_action_root, glyph, offset_x, offset_y, color, 0, 0);
        }
    }
    draw_action_status();
}

}  // namespace

bool actions_create() {
    const auto* c = components();
    const auto* adapter = ui();
    if (c == nullptr || c->panel_create == nullptr || c->button_create == nullptr ||
        c->destroy == nullptr || adapter == nullptr || adapter->set_draw_proc == nullptr)
        return false;
    g_pending_action = PendingAction::kNone;
    g_confirmation_token = 0;
    g_confirmation_open = false;
    g_status_kind = kStatusNone;
    g_status_text[0] = '\0';
    const UiRect panel = panel_rect();
    const int32_t bx = static_cast<int32_t>(panel.x + panel.w) + kActionGap;
    const int32_t by = static_cast<int32_t>(panel.y) + kActionFirstY;
    const UiPanelDesc desc{{bx, by, kActionButtonSize, kActionRootHeight},
                           {0, 0, panel.x * 2 + panel.w, 640},
                           0, 0};
    if (!succeeded(c->panel_create(&desc, nullptr, &g_action_root))) {
        actions_destroy();
        return false;
    }
    for (int32_t index = 0; index < kActionButtonCount; ++index) {
        if (!button_visible(index)) continue;
        const UiButtonDesc button{{0, static_cast<int64_t>(index) * kActionButtonStride,
                                   kActionButtonSize, kActionButtonSize},
                                  "", 0, &action_button_clicked,
                                  reinterpret_cast<void*>(static_cast<intptr_t>(index))};
        QolUiHandle handle = nullptr;
        if (!succeeded(c->button_create(&button, g_action_root, &handle)) || handle == nullptr) {
            actions_destroy();
            return false;
        }
        if (!succeeded(adapter->set_draw_proc(handle, &noop_draw))) {
            (void)c->destroy(handle);
            actions_destroy();
            return false;
        }
        g_action_buttons[index] = handle;
    }
    return true;
}

void actions_destroy() {
    // 面板销毁时关闭未完成的确认框；已回答的对话框不重复 close。
    if (g_confirmation_open && g_confirmation_token != 0) {
        const auto* api = confirmation_api();
        if (api != nullptr && api->close != nullptr) (void)api->close(g_confirmation_token);
    }
    g_confirmation_open = false;
    g_confirmation_token = 0;
    g_pending_action = PendingAction::kNone;
    const auto* c = components();
    // root destroy 释放 owned native 子树，按钮随之销毁。
    if (c != nullptr && c->destroy != nullptr && g_action_root != nullptr)
        (void)c->destroy(g_action_root);
    g_action_root = nullptr;
    for (QolUiHandle& button : g_action_buttons) button = nullptr;
    g_status_kind = kStatusNone;
    g_status_text[0] = '\0';
}

void actions_process() {
    process_pending_action();
}

void actions_draw() {
    draw_action_buttons();
}

uint32_t actions_dispatch_event(uint64_t event, uint64_t param, uint64_t param2) {
    const auto* c = components();
    if (c == nullptr || c->dispatch_native_event == nullptr || g_action_root == nullptr) return 0;
    uint32_t consumed = 0;
    (void)c->dispatch_native_event(g_action_root, event, param, param2, &consumed);
    return consumed;
}

}  // namespace qol::features::item_editor::panel_internal
