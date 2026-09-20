// 世界传送 4 选项业务层。声明式入口 patch 的 handler 转入本文件。

#include "feature.h"

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

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
};

std::array<ChoiceTarget, kTeleportChoiceCount> g_targets{};
std::array<char, kMapNameBufferSize> g_confirm_text{};
constexpr char kUnknownMapName[] = "未知地图";
ChoiceTarget g_pending_target{};

const qol::contracts::QolGameHost* game() {
    return qol::features::world_teleport::game_host();
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
    if (out == nullptr || game() == nullptr || game()->read.world.state == nullptr) return false;
    int32_t value = -1;
    if (!qol::contracts::succeeded(game()->read.world.state(&value))) return false;
    *out = value;
    return true;
}

bool read_map_record_count(int* out) {
    if (out == nullptr || game() == nullptr || game()->read_only.world.map_record_count == nullptr) {
        return false;
    }
    int32_t value = 0;
    if (!qol::contracts::succeeded(game()->read_only.world.map_record_count(&value))) return false;
    *out = value;
    return true;
}

bool read_current_map_id(int* out) {
    if (out == nullptr || game() == nullptr || game()->read_only.world.current_map_id == nullptr) {
        return false;
    }
    int32_t value = 0;
    if (!qol::contracts::succeeded(game()->read_only.world.current_map_id(&value))) return false;
    *out = value;
    return true;
}

const char* read_map_name(int map_id) {
    if (game() == nullptr || game()->read_only.world.map_name == nullptr) return nullptr;
    const char* value = nullptr;
    if (!qol::contracts::succeeded(game()->read_only.world.map_name(map_id, &value))) return nullptr;
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

    std::array<std::array<char, kMapNameBufferSize>, kChoiceCount> choice_text{};
    std::array<const char*, kChoiceCount> items{};
    for (size_t index = 0; index < kTeleportChoiceCount; ++index) {
        const int delta = kChoiceDeltas[index];
        const int target_id = world_teleport::target_map_id(current_id, delta, max_map_id);
        const bool valid = target_id >= 0 && (record_count <= 0 || target_id < record_count);
        g_targets[index] = {target_id, kChoiceCosts[index], valid};
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
        static_cast<int>(kCloseChoiceIndex), &teleport_on_select, nullptr};
    const qol::contracts::UiChoiceApi* api = ui_choice_api();
    if (api == nullptr || api->open == nullptr) {
        log_message(3, "world teleport: ui_choice capability unavailable");
        return;
    }
    if (!api->open(spec)) {
        log_message(3, "world teleport: choice open failed");
        return;
    }
    int state_id = -1;
    if (game() != nullptr && game()->read_only.choice.state_id != nullptr) {
        (void)game()->read_only.choice.state_id(&state_id);
    }
    log_message(1, "world teleport: choice opened current=%d state=%d", current_id, state_id);
}

int32_t delayed_open_choice(int64_t, void*) {
    open_choice_panel();
    return 0;
}

int32_t delayed_open_confirmation(int64_t, void* param) {
    const auto* target = static_cast<const ChoiceTarget*>(param);
    if (target == nullptr || game() == nullptr || game()->world_action.create_yesno == nullptr) {
        log_message(3, "world teleport: confirmation symbols not resolved");
        return 0;
    }
    (void)game()->world_action.create_yesno(
        g_confirm_text.data(), static_cast<uint32_t>(std::strlen(g_confirm_text.data())),
        reinterpret_cast<void*>(&teleport_confirmed), reinterpret_cast<void*>(&teleport_cancelled),
        reinterpret_cast<void*>(static_cast<intptr_t>(target->cost)));
    log_message(1, "world teleport: confirmation opened target=%d cost=%d",
                target->map_id, target->cost);
    return 0;
}

void request_open_choice() {
#if WORLD_TELEPORT_OPEN_NEXT_FRAME
    if (!schedule(delayed_open_choice, nullptr, 0)) {
        log_message(3, "world teleport: delayed choice registration failed");
    }
#else
    open_choice_panel();
#endif
}

void show_message(const char* message) {
    if (game() == nullptr || game()->world_action.show_banner == nullptr ||
        !qol::contracts::succeeded(game()->world_action.show_banner(message))) {
        log_message(3, "world teleport: instant message symbol not resolved");
        return;
    }
    log_message(1, "world teleport: instant message shown text=%s", message);
}

int32_t delayed_transfer(int64_t, void* param) {
    const auto* target = static_cast<const ChoiceTarget*>(param);
    if (target == nullptr || game() == nullptr || game()->read_only.world.money == nullptr ||
        game()->read_only.world.active_player_direction == nullptr ||
        game()->world_action.minus_money == nullptr ||
        game()->world_action.request_map_change == nullptr) {
        log_message(3, "world teleport: transfer symbols not resolved");
        return 0;
    }
    int32_t direction = -1;
    int64_t money = 0;
    if (!qol::contracts::succeeded(game()->read_only.world.active_player_direction(&direction)) ||
        direction < 0 || !qol::contracts::succeeded(game()->read_only.world.money(&money))) {
        log_message(3, "world teleport: active player unavailable");
        return 0;
    }
    if (money < target->cost || !qol::contracts::succeeded(
            game()->world_action.minus_money(static_cast<int64_t>(target->cost)))) {
        show_message("金币不足，无法传送");
        log_message(1, "world teleport: insufficient money target=%d cost=%d money=%lld",
                    target->map_id, target->cost, static_cast<long long>(money));
        return 0;
    }
    (void)game()->world_action.request_map_change(target->map_id, 0, 0, direction);
    log_message(1, "world teleport: transferred target=%d cost=%d direction=%d",
                target->map_id, target->cost, direction);
    return 0;
}

int32_t delayed_close_choice(int64_t, void* param) {
    if (game() == nullptr || game()->world_action.close_popup == nullptr ||
        game()->world_action.restore_hud == nullptr) {
        log_message(3, "world teleport: choice close symbols not resolved");
        return 0;
    }
    (void)game()->world_action.close_popup();
    (void)game()->world_action.restore_hud();
    if (!schedule(delayed_transfer, param, 1)) {
        log_message(3, "world teleport: transfer registration failed");
    }
    return 0;
}

void teleport_confirmed(void*) {
    const ChoiceTarget* target = &g_pending_target;
    if (!target->valid || game() == nullptr || game()->world_action.close_popup == nullptr) {
        log_message(3, "world teleport: confirm symbols not resolved");
        return;
    }
    if (!schedule(delayed_close_choice, const_cast<ChoiceTarget*>(target), 1)) {
        log_message(3, "world teleport: transfer registration failed");
        return;
    }
    (void)game()->world_action.close_popup();
    if (game()->world_action.restore_hud != nullptr) {
        (void)game()->world_action.restore_hud();
    }
}

void teleport_cancelled(void*) {
    if (!schedule(delayed_open_choice, nullptr, 1)) {
        log_message(3, "world teleport: choice reopen registration failed");
    }
    log_message(1, "world teleport: confirmation cancelled");
}

void teleport_on_select(int index, void*) {
    if (index < 0 || index >= static_cast<int>(kTeleportChoiceCount) ||
        game() == nullptr || game()->world_action.create_yesno == nullptr) {
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
    std::snprintf(g_confirm_text.data(), g_confirm_text.size(), "是否传送至%s？",
                  map_name(target.map_id));
    close_choice_panel();
    if (game()->world_action.restore_hud != nullptr) {
        (void)game()->world_action.restore_hud();
    }
    g_pending_target = target;
    if (!schedule(delayed_open_confirmation, &g_pending_target, 1)) {
        log_message(3, "world teleport: confirmation registration failed");
        return;
    }
    log_message(1, "world teleport: selected index=%d target=%d cost=%d",
                index, target.map_id, target.cost);
}

}  // namespace

namespace qol::features::world_teleport {

void world_teleport_call_map_name() {
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
