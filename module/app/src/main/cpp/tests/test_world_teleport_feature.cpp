#include "contracts/game_ports/choice_port.h"
#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/world_action_port.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"
#include "contracts/plugin.h"
#include "contracts/ui.h"
#include "contracts/ui_choice.h"
#include "contracts/world_facts.h"
#include "plugins/world_teleport/feature.h"
#include "plugins/world_teleport/plugin.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct ScheduledTask {
    qol::contracts::QolFrameHandler handler;
    void* context;
};

int32_t g_state = 5;
int32_t g_record_count = 20;
int32_t g_current_map_id = 5;
int32_t g_direction = 2;
int64_t g_money = 0;
int g_choice_open_calls = 0;
int g_choice_close_calls = 0;
int g_yesno_calls = 0;
int g_minus_money_calls = 0;
int g_map_change_calls = 0;
int g_add_money_calls = 0;
int64_t g_add_money_amount = 0;
bool g_map_change_fail = false;
std::string g_banner;
std::string g_last_log;
std::vector<ScheduledTask> g_tasks;
std::vector<std::string> g_choice_items;
void (*g_choice_select)(int, void*) = nullptr;
void* g_choice_user = nullptr;
void* g_yes_callback = nullptr;
void* g_no_callback = nullptr;
void* g_yes_param = nullptr;

QolResult world_state(int32_t* out) {
    *out = g_state;
    return qol::contracts::ok();
}

QolResult map_record_count(int32_t* out) {
    *out = g_record_count;
    return qol::contracts::ok();
}

QolResult current_map_id(int32_t* out) {
    *out = g_current_map_id;
    return qol::contracts::ok();
}

QolResult map_name(int32_t, const char** out) {
    *out = "测试地图";
    return qol::contracts::ok();
}

QolResult active_player_direction(int32_t* out) {
    *out = g_direction;
    return qol::contracts::ok();
}

QolResult money(int64_t* out) {
    *out = g_money;
    return qol::contracts::ok();
}

QolResult fake_log_write(int32_t, int32_t, const char*, const char* message, uint32_t) {
    if (message != nullptr) g_last_log = message;
    return qol::contracts::ok();
}

QolResult schedule_task(int32_t, qol::contracts::QolFrameHandler handler, void* context,
                        int32_t, int32_t, uint64_t* out_id) {
    if (handler == nullptr || out_id == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid task", 12);
    }
    g_tasks.push_back({handler, context});
    *out_id = g_tasks.size();
    return qol::contracts::ok();
}

const qol::contracts::WorldFactsApi kWorldFacts{
    &world_state, nullptr, &map_record_count, &current_map_id, &map_name,
    &active_player_direction, nullptr};
const qol::contracts::ItemFactsApi kItemFacts{
    static_cast<uint32_t>(sizeof(qol::contracts::ItemFactsApi)), nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &money};

bool ui_choice_open(const qol::contracts::UiChoiceSpec& spec) {
    ++g_choice_open_calls;
    g_choice_items.clear();
    for (int i = 0; i < spec.count; ++i) g_choice_items.emplace_back(spec.items[i]);
    g_choice_select = spec.on_select;
    g_choice_user = spec.user;
    return true;
}

bool ui_choice_active() { return false; }

void ui_choice_close() { ++g_choice_close_calls; }

const qol::contracts::UiChoiceApi kUiChoice{&ui_choice_open, &ui_choice_active,
                                            &ui_choice_close};

QolResult read_layers(qol::contracts::QolUiLayerVisitFn visit, void* user) {
    if (visit == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid visitor", 15);
    }
    const qol::contracts::UiLayerInfo dialog{
        "world_teleport.confirm",
        static_cast<uint32_t>(qol::contracts::UiLayerType::kDialog)};
    visit(&dialog, user);
    return qol::contracts::ok();
}

const qol::contracts::UiStateApi kUiState{nullptr, nullptr, nullptr, &read_layers};

QolResult require_capability(const char* id, QolVersionRange, const void** out_iface) {
    if (std::strcmp(id, qol::contracts::kWorldFactsCapabilityId) == 0) {
        *out_iface = &kWorldFacts;
    } else if (std::strcmp(id, qol::contracts::kItemFactsCapabilityId) == 0) {
        *out_iface = &kItemFacts;
    } else if (std::strcmp(id, qol::contracts::kUiChoiceCapabilityId) == 0) {
        *out_iface = &kUiChoice;
    } else if (std::strcmp(id, qol::contracts::kUiStateCapabilityId) == 0) {
        *out_iface = &kUiState;
    } else {
        *out_iface = nullptr;
        return qol::contracts::error(kQolErrNotFound, "missing capability", 18);
    }
    return qol::contracts::ok();
}

QolResult create_yesno(const char*, uint32_t, void* yes, void* no, void* param) {
    ++g_yesno_calls;
    g_yes_callback = yes;
    g_no_callback = no;
    g_yes_param = param;
    return qol::contracts::ok();
}

QolResult minus_money(int64_t) {
    ++g_minus_money_calls;
    return qol::contracts::ok();
}

QolResult request_map_change(int32_t, int32_t, int32_t, int32_t) {
    ++g_map_change_calls;
    if (g_map_change_fail) {
        return qol::contracts::error(kQolErrNotReady, "map change not ready", 21);
    }
    return qol::contracts::ok();
}

QolResult add_money(int64_t amount) {
    ++g_add_money_calls;
    g_add_money_amount = amount;
    return qol::contracts::ok();
}

QolResult close_popup() { return qol::contracts::ok(); }

QolResult restore_hud() { return qol::contracts::ok(); }

QolResult show_banner(const char* text) {
    g_banner = text;
    return qol::contracts::ok();
}

void run_next_task() {
    assert(!g_tasks.empty());
    const ScheduledTask task = g_tasks.front();
    g_tasks.erase(g_tasks.begin());
    (void)task.handler(0, task.context);
}

void reset_observations() {
    g_choice_open_calls = 0;
    g_choice_close_calls = 0;
    g_yesno_calls = 0;
    g_minus_money_calls = 0;
    g_map_change_calls = 0;
    g_add_money_calls = 0;
    g_add_money_amount = 0;
    g_map_change_fail = false;
    g_banner.clear();
    g_last_log.clear();
    g_tasks.clear();
    g_choice_items.clear();
    g_choice_select = nullptr;
    g_choice_user = nullptr;
    g_yes_callback = nullptr;
    g_no_callback = nullptr;
    g_yes_param = nullptr;
}

}  // namespace

int main() {
    qol::contracts::QolHostSdk sdk{};
    sdk.log.write = &fake_log_write;
    sdk.scheduler.add = &schedule_task;
    sdk.capabilities.require = &require_capability;

    qol::contracts::QolWorldActionPort actions{};
    actions.abi_version = qol::contracts::kQolPortWorldActionVersion;
    actions.actions.create_yesno = &create_yesno;
    actions.actions.minus_money = &minus_money;
    actions.actions.request_map_change = &request_map_change;
    actions.actions.close_popup = &close_popup;
    actions.actions.restore_hud = &restore_hud;
    actions.actions.show_banner = &show_banner;
    actions.actions.add_money = &add_money;

    const void* ports[] = {&actions};
    qol::contracts::PluginContext context{};
    context.struct_size = sizeof(context);
    context.sdk_major = qol::contracts::kHostSdkMajor;
    context.sdk = &sdk;
    context.game = nullptr;  // 私有端口迁移后插件不再依赖聚合 Game Host
    context.ports = ports;
    context.port_count = 1;

    const auto& descriptor = qol::features::world_teleport::plugin_descriptor();
    assert(descriptor.install(context) == 0);

    reset_observations();
    g_current_map_id = 5;
    g_record_count = 20;
    g_money = 0;
    qol::features::world_teleport::world_teleport_call_map_name();
    assert(g_choice_open_calls == 1);
    assert(g_last_log.find("top=world_teleport.confirm") != std::string::npos);
    assert(g_choice_items.size() == 5);
    assert(g_choice_select != nullptr);
    g_choice_select(0, g_choice_user);
    assert(g_tasks.size() == 1);
    run_next_task();
    assert(g_yesno_calls == 1);
    assert(g_yes_callback != nullptr);
    auto confirm = reinterpret_cast<void (*)(void*)>(g_yes_callback);
    confirm(g_yes_param);
    assert(g_tasks.size() == 1);
    run_next_task();
    assert(g_tasks.size() == 1);
    run_next_task();
    assert(g_banner == "金币不足，无法传送");
    assert(g_minus_money_calls == 0);
    assert(g_map_change_calls == 0);

    reset_observations();
    g_current_map_id = -1;
    g_record_count = 20;
    qol::features::world_teleport::world_teleport_call_map_name();
    assert(g_choice_open_calls == 1);
    assert(g_choice_items.size() == 5);
    assert(g_choice_items[0].find("地图数据不足") != std::string::npos);
    assert(g_choice_select != nullptr);
    g_choice_select(0, g_choice_user);
    assert(g_banner == "目标地图无效，无法传送");
    assert(g_tasks.empty());
    assert(g_yesno_calls == 0);
    assert(g_minus_money_calls == 0);
    assert(g_map_change_calls == 0);

    // WT-03：扣费成功但切图失败 → 全额退款 + 失败横幅
    reset_observations();
    g_current_map_id = 5;
    g_record_count = 20;
    g_money = 10000;
    g_map_change_fail = true;
    qol::features::world_teleport::world_teleport_call_map_name();
    assert(g_choice_select != nullptr);
    g_choice_select(0, g_choice_user);
    run_next_task();
    confirm = reinterpret_cast<void (*)(void*)>(g_yes_callback);
    confirm(g_yes_param);
    run_next_task();
    run_next_task();
    assert(g_minus_money_calls == 1);
    assert(g_map_change_calls == 1);
    assert(g_add_money_calls == 1);
    assert(g_add_money_amount == 300);
    assert(g_banner == "传送失败，已退还金币");

    // WT-04：停用后已排队的帧任务不再执行
    reset_observations();
    g_current_map_id = 5;
    g_record_count = 20;
    g_money = 10000;
    qol::features::world_teleport::world_teleport_call_map_name();
    assert(g_choice_select != nullptr);
    g_choice_select(0, g_choice_user);
    assert(!g_tasks.empty());
    descriptor.on_disabled(context);
    while (!g_tasks.empty()) run_next_task();
    assert(g_yesno_calls == 0);
    assert(g_minus_money_calls == 0);
    assert(g_map_change_calls == 0);
    assert(g_banner.empty());

    descriptor.deactivate(context);
    return 0;
}
