#include "contracts/game_host.h"
#include "contracts/game_ports/choice_port.h"
#include "contracts/game_ports/ports.h"
#include "contracts/game_ports/world_action_port.h"
#include "contracts/host_sdk.h"
#include "contracts/item_facts.h"
#include "contracts/plugin.h"
#include "contracts/static_data.h"
#include "contracts/ui.h"
#include "contracts/ui_choice.h"
#include "contracts/world_facts.h"
#include "plugins/world_teleport/feature.h"
#include "plugins/world_teleport/plugin.h"
#include "plugins/world_teleport/rules.h"

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
int32_t g_series = 3;  // GameSeries 序数：默认 Monster（确认+扣费路径为既有用例基线）。
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

// QolTargetVariant.id = GameSeries 序数（0=Unknown、1=Original、2=Overhaul、3=Monster、4=C18）。
QolResult query_variant(qol::contracts::QolTargetVariant* out) {
    out->id = static_cast<uint32_t>(g_series);
    out->capabilities = 0;
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

constexpr int32_t kStaticTileDim = qol::contracts::kStaticMapTileDim;
uint8_t g_static_tiles[qol::contracts::kStaticMapTileSize]{};
int32_t g_missing_static_map_id = -1;
bool g_static_available = true;
int g_static_info_calls = 0;
int g_static_tiles_calls = 0;

QolResult static_map_name(int32_t, char* out, uint32_t capacity, uint32_t* out_size) {
    static constexpr char kName[] = "静态地图";
    if (out == nullptr || out_size == nullptr || capacity < sizeof(kName)) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid map name out", 21);
    }
    std::memcpy(out, kName, sizeof(kName));
    *out_size = static_cast<uint32_t>(sizeof(kName) - 1);
    return qol::contracts::ok();
}

QolResult static_map_exits(int32_t, qol::contracts::QolStaticMapExit*, uint32_t,
                           uint32_t* out_count) {
    if (out_count == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid exits out", 17);
    }
    *out_count = 0;
    return qol::contracts::ok();
}

QolResult static_map_info(int32_t map_id, qol::contracts::QolStaticMapInfo* out) {
    ++g_static_info_calls;
    if (out == nullptr) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid map info", 16);
    }
    if (map_id == g_missing_static_map_id) {
        return qol::contracts::error(kQolErrNotFound, "map tiles not found", 19);
    }
    out->map_id = map_id;
    out->width = kStaticTileDim;
    out->height = kStaticTileDim;
    return qol::contracts::ok();
}

QolResult static_map_tiles(int32_t map_id, uint8_t* out_tiles, uint32_t capacity,
                           uint32_t* out_size) {
    ++g_static_tiles_calls;
    if (out_tiles == nullptr || out_size == nullptr ||
        capacity < qol::contracts::kStaticMapTileSize) {
        return qol::contracts::error(kQolErrInvalidArgument, "invalid map tiles out", 22);
    }
    if (map_id == g_missing_static_map_id) {
        return qol::contracts::error(kQolErrNotFound, "map tiles not found", 19);
    }
    std::memcpy(out_tiles, g_static_tiles, qol::contracts::kStaticMapTileSize);
    *out_size = qol::contracts::kStaticMapTileSize;
    return qol::contracts::ok();
}

const qol::contracts::QolStaticDataApi kStaticData{
    &static_map_name, &static_map_exits, &static_map_info, &static_map_tiles};

// 变体查询桩：id = GameSeries 序数（g_series 控制当前用例的变体）。
qol::contracts::QolGameHost kGameHost{};

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
    } else if (std::strcmp(id, qol::contracts::kStaticDataCapabilityId) == 0 &&
               g_static_available) {
        *out_iface = &kStaticData;
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

int32_t g_last_map_x = 0;
int32_t g_last_map_y = 0;
int32_t g_last_map_direction = 0;

QolResult request_map_change(int32_t, int32_t x, int32_t y, int32_t direction) {
    ++g_map_change_calls;
    g_last_map_x = x;
    g_last_map_y = y;
    g_last_map_direction = direction;
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
    g_last_map_x = 0;
    g_last_map_y = 0;
    g_last_map_direction = 0;
    g_static_info_calls = 0;
    g_static_tiles_calls = 0;
    g_missing_static_map_id = -1;
    std::memset(g_static_tiles, 0, sizeof(g_static_tiles));
    g_static_available = true;
    g_add_money_calls = 0;
    g_add_money_amount = 0;
    g_map_change_fail = false;
    g_series = 3;  // Monster：既有确认+扣费用例的基线变体（unknown/free 有专属用例）
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
    kGameHost.target.query_variant = &query_variant;
    qol::contracts::PluginContext context{};
    context.struct_size = sizeof(context);
    context.sdk_major = qol::contracts::kHostSdkMajor;
    context.sdk = &sdk;
    context.game = &kGameHost;  // 变体序列经 QolGameHost.target.query_variant 读取
    context.ports = ports;
    context.port_count = 1;

    const auto& descriptor = qol::features::world_teleport::plugin_descriptor();

    // rules::pick_landing：全通中点、X+ 优先、全阻挡失败。
    {
        uint8_t tiles[qol::contracts::kStaticMapTileSize]{};
        int land_x = -1;
        int land_y = -1;
        assert(world_teleport::pick_landing(tiles, 64, 64, &land_x, &land_y));
        assert(land_x == 32 && land_y == 32);
        tiles[32 * kStaticTileDim + 32] = 0x08;
        assert(world_teleport::pick_landing(tiles, 64, 64, &land_x, &land_y));
        assert(land_x == 33 && land_y == 32);
        std::memset(tiles, 0x08, sizeof(tiles));
        assert(!world_teleport::pick_landing(tiles, 64, 64, &land_x, &land_y));
        assert(!world_teleport::pick_landing(nullptr, 64, 64, &land_x, &land_y));
        assert(!world_teleport::pick_landing(tiles, 0, 64, &land_x, &land_y));
    }

    assert(descriptor.install(context) == 0);

    reset_observations();
    g_current_map_id = 5;
    g_record_count = 20;
    g_money = 0;
    qol::features::world_teleport::world_teleport_call_map_name();
    assert(g_choice_open_calls == 1);
    assert(g_banner == "测试地图");
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

    // 变体分流：C18 免费直传——无确认框、无扣费，余额不足仍传送。
    reset_observations();
    g_series = 4;  // C18：免费直传
    g_current_map_id = 5;
    g_record_count = 20;
    g_money = 0;
    qol::features::world_teleport::world_teleport_call_map_name();
    assert(g_choice_select != nullptr);
    g_banner.clear();
    g_choice_select(0, g_choice_user);
    assert(g_yesno_calls == 0);
    assert(g_tasks.size() == 1);
    run_next_task();
    assert(g_tasks.size() == 1);
    run_next_task();
    assert(g_yesno_calls == 0);
    assert(g_minus_money_calls == 0);
    assert(g_map_change_calls == 1);
    assert(g_banner.empty());
    // c18 走其进图选点门控：(0,0) + dir|0x8000，不调用 static.data。
    assert(g_last_map_x == 0);
    assert(g_last_map_y == 0);
    assert((g_last_map_direction & 0x8000) != 0);
    assert(g_static_info_calls == 0);
    assert(g_static_tiles_calls == 0);

    // 未知变体免费直传（不确认、不扣费），落点走 static.data + 插件选点（全通矩阵中点为 (32,32)）。
    reset_observations();
    g_series = 0;  // Unknown
    g_current_map_id = 5;
    g_record_count = 20;
    g_money = 10000;
    qol::features::world_teleport::world_teleport_call_map_name();
    assert(g_choice_select != nullptr);
    g_choice_select(0, g_choice_user);
    assert(g_tasks.size() == 1);
    run_next_task();
    assert(g_yesno_calls == 0);
    run_next_task();
    assert(g_minus_money_calls == 0);
    assert(g_map_change_calls == 1);
    assert(g_static_info_calls > 0);
    assert(g_static_tiles_calls > 0);
    assert(g_last_map_x == 32);
    assert(g_last_map_y == 32);
    assert((g_last_map_direction & 0x8000) == 0);

    // 未知变体 + 中点阻挡 → X+ 方向最近可站立格 (33,32)。
    reset_observations();
    g_static_tiles[32 * kStaticTileDim + 32] = 0x08;
    g_series = 0;  // Unknown
    g_current_map_id = 5;
    g_record_count = 20;
    g_money = 10000;
    qol::features::world_teleport::world_teleport_call_map_name();
    assert(g_choice_select != nullptr);
    g_choice_select(0, g_choice_user);
    run_next_task();
    run_next_task();
    assert(g_map_change_calls == 1);
    assert(g_last_map_x == 33);
    assert(g_last_map_y == 32);

    // 目标图静态数据缺失 → 选项不可用（错误可见），不得以 (0,0) 静默传送。
    reset_observations();
    g_missing_static_map_id = 6;  // 当前 5，索引 0 的目标
    g_series = 0;  // Unknown
    g_current_map_id = 5;
    g_record_count = 20;
    g_money = 10000;
    qol::features::world_teleport::world_teleport_call_map_name();
    assert(g_choice_items.size() == 5);
    assert(g_choice_items[0].find("地图数据缺失") != std::string::npos);
    assert(g_choice_select != nullptr);
    g_choice_select(0, g_choice_user);
    assert(g_banner == "目标地图无效，无法传送");
    assert(g_map_change_calls == 0);
    assert(g_tasks.empty());

    // 变体分流：monster 保持确认框 + 扣费路径，落点 (0,0) 交其进图补丁校正。
    reset_observations();
    g_series = 3;  // Monster：确认+扣费
    g_current_map_id = 5;
    g_record_count = 20;
    g_money = 10000;
    qol::features::world_teleport::world_teleport_call_map_name();
    assert(g_choice_select != nullptr);
    g_choice_select(0, g_choice_user);
    assert(g_tasks.size() == 1);
    run_next_task();
    assert(g_yesno_calls == 1);
    confirm = reinterpret_cast<void (*)(void*)>(g_yes_callback);
    assert(confirm != nullptr);
    confirm(g_yes_param);
    run_next_task();
    run_next_task();
    assert(g_minus_money_calls == 1);
    assert(g_map_change_calls == 1);
    assert(g_last_map_x == 0);
    assert(g_last_map_y == 0);
    assert((g_last_map_direction & 0x8000) == 0);
    assert(g_static_info_calls == 0);
    assert(g_static_tiles_calls == 0);

    // WT-04：停用后已排队的帧任务不再执行
    reset_observations();
    g_current_map_id = 5;
    g_record_count = 20;
    g_money = 10000;
    qol::features::world_teleport::world_teleport_call_map_name();
    assert(g_choice_select != nullptr);
    assert(g_banner == "测试地图");
    g_choice_select(0, g_choice_user);
    assert(!g_tasks.empty());
    // 入口横幅已写入；清空后验证停用的帧任务不再产生任何横幅。
    g_banner.clear();
    descriptor.on_disabled(context);
    while (!g_tasks.empty()) run_next_task();
    assert(g_yesno_calls == 0);
    assert(g_minus_money_calls == 0);
    assert(g_map_change_calls == 0);
    assert(g_banner.empty());

    descriptor.deactivate(context);
    return 0;
}
