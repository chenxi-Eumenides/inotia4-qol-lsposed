// 世界传送 4 选项业务层。UIPlay_CallMapName 入口 hook 的业务入口转入本文件。

#include "feature.h"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../contracts/ui.h"
#include "../../contracts/ui_choice.h"
#include "plugin.h"
#include "rules.h"

namespace {

constexpr size_t kMapNameBufferSize = 256;
constexpr size_t kTeleportChoiceCount = 4;
constexpr size_t kChoiceCount = 5;
constexpr size_t kCloseChoiceIndex = kChoiceCount - 1;
constexpr int kChoiceDeltas[kTeleportChoiceCount] = {1, 10, -1, -10};
constexpr int kChoiceCosts[kTeleportChoiceCount] = {300, 3000, 300, 3000};
constexpr int kWorldState = 5;

#ifndef WORLD_TELEPORT_OPEN_NEXT_FRAME
#define WORLD_TELEPORT_OPEN_NEXT_FRAME 0
#endif

struct ChoiceTarget {
    int map_id = 0;
    int cost = 0;
    bool valid = false;
    bool charge = true;
};

std::array<ChoiceTarget, kTeleportChoiceCount> g_targets{};
std::array<char, kMapNameBufferSize> g_confirm_text{};
constexpr char kUnknownMapName[] = "未知地图";
ChoiceTarget g_pending_target{};

const qol::contracts::QolWorldActionApi* world_actions() {
    const auto* port = qol::features::world_teleport::world_action_port();
    return port != nullptr ? &port->actions : nullptr;
}

// 停用代次：on_disabled/deactivate 递增，使已排队的帧任务与游戏回调失效。
// 只在游戏主线程读写；scheduler 回调与 UI 回调均在主线程。
std::atomic<uint32_t> g_op_generation{0};
uint32_t g_chain_generation = 0;

bool chain_still_valid() {
    return g_chain_generation == g_op_generation.load(std::memory_order_acquire);
}

void log_message(int32_t level, const char* format, ...) {
    const qol::contracts::QolHostSdk* sdk = qol::features::world_teleport::host_sdk();
    if (sdk == nullptr || sdk->log.write == nullptr || format == nullptr) return;
    char message[qol::contracts::kMessageCap + 1]{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (written < 0) return;
    const uint32_t length = static_cast<uint32_t>(written) < qol::contracts::kMessageCap
        ? static_cast<uint32_t>(written) : qol::contracts::kMessageCap;
    (void)sdk->log.write(level, 12, __FILE__, message, length);
}

bool read_state(int* out) {
    const auto* facts = qol::features::world_teleport::world_facts();
    if (out == nullptr || facts == nullptr || facts->state == nullptr) return false;
    int32_t value = -1;
    if (!qol::contracts::succeeded(facts->state(&value))) return false;
    *out = value;
    return true;
}

// series 缺失或读取失败时保持 kWorldFactsSeriesUnknown，由调用方按收费策略处理。
bool read_series(int* out) {
    const auto* facts = qol::features::world_teleport::world_facts();
    if (out == nullptr || facts == nullptr || facts->series == nullptr) return false;
    int32_t value = qol::contracts::kWorldFactsSeriesUnknown;
    if (!qol::contracts::succeeded(facts->series(&value))) return false;
    *out = value;
    return true;
}

bool read_map_record_count(int* out) {
    const auto* facts = qol::features::world_teleport::world_facts();
    if (out == nullptr || facts == nullptr || facts->map_record_count == nullptr) {
        return false;
    }
    int32_t value = 0;
    if (!qol::contracts::succeeded(facts->map_record_count(&value))) return false;
    *out = value;
    return true;
}

bool read_current_map_id(int* out) {
    const auto* facts = qol::features::world_teleport::world_facts();
    if (out == nullptr || facts == nullptr || facts->current_map_id == nullptr) {
        return false;
    }
    int32_t value = 0;
    if (!qol::contracts::succeeded(facts->current_map_id(&value))) return false;
    *out = value;
    return true;
}

const char* read_map_name(int map_id) {
    const auto* facts = qol::features::world_teleport::world_facts();
    if (facts == nullptr || facts->map_name == nullptr) return nullptr;
    const char* value = nullptr;
    if (!qol::contracts::succeeded(facts->map_name(map_id, &value))) return nullptr;
    return value;
}

bool schedule(qol::contracts::QolFrameHandler handler, void* ctx, int interval) {
    const qol::contracts::QolHostSdk* sdk = qol::features::world_teleport::host_sdk();
    if (sdk == nullptr || sdk->scheduler.add == nullptr) return false;
    uint64_t id = 0;
    const QolResult result = sdk->scheduler.add(1, handler, ctx, interval, 1, &id);
    return qol::contracts::succeeded(result) && id != 0;
}

const qol::contracts::UiChoiceApi* ui_choice_api() {
    const qol::contracts::QolHostSdk* sdk = qol::features::world_teleport::host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = sdk->capabilities.require(
        qol::contracts::kUiChoiceCapabilityId,
        QolVersionRange{static_cast<uint16_t>(qol::contracts::kUiChoiceVersionMajor),
                        static_cast<uint16_t>(qol::contracts::kUiChoiceVersionMinor)}, &iface);
    return qol::contracts::succeeded(result)
        ? static_cast<const qol::contracts::UiChoiceApi*>(iface) : nullptr;
}

void close_choice_panel() {
    const qol::contracts::UiChoiceApi* api = ui_choice_api();
    if (api != nullptr && api->close != nullptr) api->close();
}

void teleport_confirmed(void* param);
void teleport_cancelled(void* param);
void teleport_on_select(int index, void* user);

const char* map_name(int map_id) {
    const char* text = read_map_name(map_id);
    return text != nullptr ? text : kUnknownMapName;
}

struct LayerLogContext {
    char id[qol::contracts::kUiStateMaxPanelIdBytes]{};
    uint32_t type = 0;
    bool found = false;
};

void collect_top_layer(const qol::contracts::UiLayerInfo* layer, void* user) {
    auto* out = static_cast<LayerLogContext*>(user);
    if (out == nullptr || out->found || layer == nullptr || layer->id == nullptr) return;
    std::snprintf(out->id, sizeof(out->id), "%s", layer->id);
    out->type = layer->type;
    out->found = true;
}

// 诊断：记录当前 UI 栈顶层的 id 与类型；能力缺失或失败只降级为 top=-，不影响传送流程。
void log_ui_top_layer(int current_id) {
    const auto* sdk = qol::features::world_teleport::host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) {
        log_message(1, "world teleport: choice opened current=%d top=-", current_id);
        return;
    }
    const void* iface = nullptr;
    if (!qol::contracts::succeeded(sdk->capabilities.require(
            qol::contracts::kUiStateCapabilityId,
            QolVersionRange{static_cast<uint16_t>(qol::contracts::kUiStateVersionMajor),
                            static_cast<uint16_t>(qol::contracts::kUiStateVersionMinor)},
            &iface)) ||
        iface == nullptr) {
        log_message(1, "world teleport: choice opened current=%d top=-", current_id);
        return;
    }
    const auto* api = static_cast<const qol::contracts::UiStateApi*>(iface);
    if (api->read_layers == nullptr) {
        log_message(1, "world teleport: choice opened current=%d top=-", current_id);
        return;
    }
    LayerLogContext top{};
    (void)api->read_layers(&collect_top_layer, &top);
    log_message(1, "world teleport: choice opened current=%d top=%s type=%u",
                current_id, top.found ? top.id : "-", top.type);
}

void open_choice_panel() {
    int current_id = 0;
    int record_count = 0;
    if (!read_current_map_id(&current_id) || !read_map_record_count(&record_count)) {
        log_message(3, "world teleport: choice symbols not resolved");
        return;
    }
    const int max_map_id = world_teleport::max_map_id_from_record_count(record_count);
    log_message(1, "world teleport: map records count=%d max_map_id=%d current=%d",
                record_count, max_map_id, current_id);

    // 选择面板统一；回调按变体分流：C18/原版免费直传，monster/大修/未知走确认与扣费。
    int series = qol::contracts::kWorldFactsSeriesUnknown;
    (void)read_series(&series);
    const bool charge = !(series == qol::contracts::kWorldFactsSeriesC18 ||
                          series == qol::contracts::kWorldFactsSeriesOriginal);

    std::array<std::array<char, kMapNameBufferSize>, kChoiceCount> choice_text{};
    std::array<const char*, kChoiceCount> items{};
    for (size_t index = 0; index < kTeleportChoiceCount; ++index) {
        const int delta = kChoiceDeltas[index];
        const int target_id = world_teleport::target_map_id(current_id, delta, max_map_id);
        const bool valid = target_id >= 0 && (record_count <= 0 || target_id < record_count);
        g_targets[index] = {target_id, kChoiceCosts[index], valid, charge};
        const char* name = valid ? map_name(target_id) : "不可用";
        const char* sign = delta > 0 ? "+" : "-";
        if (valid) {
            std::snprintf(choice_text[index].data(), choice_text[index].size(),
                          "%s(id%s%d)", name, sign, delta > 0 ? delta : -delta);
        } else {
            std::snprintf(choice_text[index].data(), choice_text[index].size(),
                          "%s（地图数据不足）", name);
        }
        items[index] = choice_text[index].data();
    }
    std::snprintf(choice_text[kCloseChoiceIndex].data(), choice_text[kCloseChoiceIndex].size(),
                  "关闭");
    items[kCloseChoiceIndex] = choice_text[kCloseChoiceIndex].data();
    std::array<char, kMapNameBufferSize> title{};
    std::snprintf(title.data(), title.size(), "当前地图：%s(id:%d)", map_name(current_id), current_id);

    const qol::contracts::UiChoiceSpec spec{
        items.data(), static_cast<int>(kChoiceCount), title.data(),
        static_cast<int>(kCloseChoiceIndex), &teleport_on_select, nullptr,
        "world_teleport.confirm"};
    const qol::contracts::UiChoiceApi* api = ui_choice_api();
    if (api == nullptr || api->open == nullptr) {
        log_message(3, "world teleport: ui_choice capability unavailable");
        return;
    }
    if (!api->open(spec)) {
        log_message(3, "world teleport: choice open failed");
        return;
    }
    log_ui_top_layer(current_id);
}

int32_t delayed_open_choice(int64_t, void*) {
    if (!chain_still_valid()) return 0;
    open_choice_panel();
    return 0;
}

int32_t delayed_open_confirmation(int64_t, void* param) {
    if (!chain_still_valid()) return 0;
    const auto* target = static_cast<const ChoiceTarget*>(param);
    const auto* actions = world_actions();
    if (target == nullptr || actions == nullptr || actions->create_yesno == nullptr) {
        log_message(3, "world teleport: confirmation symbols not resolved");
        return 0;
    }
    if (!qol::contracts::succeeded(actions->create_yesno(
            g_confirm_text.data(), static_cast<uint32_t>(std::strlen(g_confirm_text.data())),
            reinterpret_cast<void*>(&teleport_confirmed),
            reinterpret_cast<void*>(&teleport_cancelled),
            reinterpret_cast<void*>(static_cast<intptr_t>(target->cost))))) {
        log_message(3, "world teleport: confirmation open failed target=%d", target->map_id);
        return 0;
    }
    log_message(1, "world teleport: confirmation opened target=%d cost=%d",
                target->map_id, target->cost);
    return 0;
}

void request_open_choice() {
    g_chain_generation = g_op_generation.load(std::memory_order_acquire);
#if WORLD_TELEPORT_OPEN_NEXT_FRAME
    if (!schedule(delayed_open_choice, nullptr, 0)) {
        log_message(3, "world teleport: delayed choice registration failed");
    }
#else
    open_choice_panel();
#endif
}

void show_message(const char* message) {
    const auto* actions = world_actions();
    if (actions == nullptr || actions->show_banner == nullptr ||
        !qol::contracts::succeeded(actions->show_banner(message))) {
        log_message(3, "world teleport: instant message symbol not resolved");
        return;
    }
    log_message(1, "world teleport: instant message shown text=%s", message);
}

int32_t delayed_transfer(int64_t, void* param) {
    if (!chain_still_valid()) return 0;
    const auto* target = static_cast<const ChoiceTarget*>(param);
    const auto* facts = qol::features::world_teleport::world_facts();
    const auto* item_facts = qol::features::world_teleport::item_facts();
    const auto* actions = world_actions();
    if (target == nullptr || facts == nullptr || actions == nullptr ||
        facts->active_player_direction == nullptr ||
        actions->request_map_change == nullptr) {
        log_message(3, "world teleport: transfer symbols not resolved");
        return 0;
    }
    if (target->charge && (item_facts == nullptr || item_facts->money == nullptr ||
                           actions->minus_money == nullptr)) {
        log_message(3, "world teleport: transfer symbols not resolved");
        return 0;
    }
    int32_t direction = -1;
    if (!qol::contracts::succeeded(facts->active_player_direction(&direction)) ||
        direction < 0) {
        log_message(3, "world teleport: active player unavailable");
        return 0;
    }
    const int cost = target->charge ? target->cost : 0;
    if (target->charge) {
        int64_t money = 0;
        if (!qol::contracts::succeeded(item_facts->money(&money))) {
            log_message(3, "world teleport: active player unavailable");
            return 0;
        }
        if (money < target->cost) {
            show_message("金币不足，无法传送");
            log_message(1, "world teleport: insufficient money target=%d cost=%d money=%lld",
                        target->map_id, target->cost, static_cast<long long>(money));
            return 0;
        }
        if (!qol::contracts::succeeded(actions->minus_money(static_cast<int64_t>(target->cost)))) {
            show_message("扣费失败，无法传送");
            log_message(3, "world teleport: charge failed target=%d cost=%d",
                        target->map_id, target->cost);
            return 0;
        }
    }
    if (!qol::contracts::succeeded(
            actions->request_map_change(target->map_id, 0, 0, direction))) {
        if (!target->charge) {
            show_message("传送失败");
            log_message(3, "world teleport: transfer failed target=%d cost=0",
                        target->map_id);
            return 0;
        }
        const bool refunded = actions->add_money != nullptr &&
            qol::contracts::succeeded(actions->add_money(static_cast<int64_t>(target->cost)));
        show_message(refunded ? "传送失败，已退还金币" : "传送失败，退款异常");
        log_message(3, "world teleport: transfer failed target=%d cost=%d refunded=%d",
                    target->map_id, target->cost, refunded ? 1 : 0);
        return 0;
    }
    log_message(1, "world teleport: transferred target=%d cost=%d",
                target->map_id, cost);
    return 0;
}

int32_t delayed_close_choice(int64_t, void* param) {
    if (!chain_still_valid()) return 0;
    const auto* actions = world_actions();
    if (actions == nullptr || actions->close_popup == nullptr ||
        actions->restore_hud == nullptr) {
        log_message(3, "world teleport: choice close symbols not resolved");
        return 0;
    }
    (void)actions->close_popup();
    (void)actions->restore_hud();
    if (!schedule(delayed_transfer, param, 1)) {
        log_message(3, "world teleport: transfer registration failed");
    }
    return 0;
}

void teleport_confirmed(void*) {
    if (!chain_still_valid()) return;
    const ChoiceTarget* target = &g_pending_target;
    const auto* actions = world_actions();
    if (!target->valid || actions == nullptr || actions->close_popup == nullptr) {
        log_message(3, "world teleport: confirm symbols not resolved");
        return;
    }
    if (!schedule(delayed_close_choice, const_cast<ChoiceTarget*>(target), 1)) {
        log_message(3, "world teleport: transfer registration failed");
        return;
    }
    (void)actions->close_popup();
    if (actions->restore_hud != nullptr) {
        (void)actions->restore_hud();
    }
}

void teleport_cancelled(void*) {
    if (!chain_still_valid()) return;
    if (!schedule(delayed_open_choice, nullptr, 1)) {
        log_message(3, "world teleport: choice reopen registration failed");
    }
    log_message(1, "world teleport: confirmation cancelled");
}

void teleport_on_select(int index, void*) {
    if (!chain_still_valid()) return;
    const auto* actions = world_actions();
    if (index < 0 || index >= static_cast<int>(kTeleportChoiceCount) || actions == nullptr) {
        close_choice_panel();
        log_message(3, "world teleport: confirm symbols not resolved");
        return;
    }
    const ChoiceTarget& target = g_targets[static_cast<size_t>(index)];
    if (!target.valid) {
        show_message("目标地图无效，无法传送");
        close_choice_panel();
        log_message(2, "world teleport: invalid target index=%d map_id=%d", index, target.map_id);
        return;
    }
    g_pending_target = target;
    if (!target.charge) {
        // 免费直传：跳过确认文案与确认框，关面板后直接进入切图调度。
        close_choice_panel();
        if (actions->restore_hud != nullptr) {
            (void)actions->restore_hud();
        }
        if (!schedule(delayed_close_choice, &g_pending_target, 1)) {
            log_message(3, "world teleport: transfer registration failed");
            return;
        }
        log_message(1, "world teleport: selected index=%d target=%d cost=0",
                    index, target.map_id);
        return;
    }
    if (actions->create_yesno == nullptr) {
        close_choice_panel();
        log_message(3, "world teleport: confirm symbols not resolved");
        return;
    }
    std::snprintf(g_confirm_text.data(), g_confirm_text.size(), "是否传送至%s？",
                  map_name(target.map_id));
    close_choice_panel();
    if (actions->restore_hud != nullptr) {
        (void)actions->restore_hud();
    }
    if (!schedule(delayed_open_confirmation, &g_pending_target, 1)) {
        log_message(3, "world teleport: confirmation registration failed");
        return;
    }
    log_message(1, "world teleport: selected index=%d target=%d cost=%d",
                index, target.map_id, target.cost);
}

}  // namespace

namespace qol::features::world_teleport {

void invalidate_operations() {
    g_op_generation.fetch_add(1, std::memory_order_release);
}

void world_teleport_call_map_name() {
    // 原函数入口即写地图名横幅；入口 hook 接管后须在业务分支前复刻该副作用。
    int banner_map_id = 0;
    if (!read_current_map_id(&banner_map_id)) {
        log_message(3, "world teleport: banner map id unavailable");
    } else {
        const char* banner_name = read_map_name(banner_map_id);
        if (banner_name != nullptr) {
            show_message(banner_name);
        } else {
            log_message(3, "world teleport: banner map id unavailable");
        }
    }
    int state = -1;
    if (!read_state(&state)) {
        log_message(3, "world teleport: state unavailable");
        return;
    }
    log_message(1, "world teleport: CallMapName intercepted state=%d", state);
    if (state != kWorldState) {
        log_message(2, "world teleport: ignored CallMapName outside world state=%d", state);
        return;
    }
    request_open_choice();
}

}  // namespace qol::features::world_teleport
