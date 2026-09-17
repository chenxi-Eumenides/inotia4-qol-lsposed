// world_teleport feature.cpp —— 世界传送 4 选项业务层（原 feature/world_teleport/world_teleport.cpp）。
//
// 触发：游戏踩到地图边界传送点（UIPlay_CallMapName 尾部）→ plugin.cpp 的入口 patch 转入
// world_teleport_call_map_name；世界态下弹出原生 UICHOICE 面板（±1/±10/关闭），
// 选择后 YesNo 确认 → 查钱扣费 → MAPCHANGE_Set + GAMESTATE_SetState(3) 切图。
//
// 全部游戏读取（MAPINFOBASE/MEMORYTEXT/当前地图/切图/金币/弹窗/横幅/HUD gate）经
// game/access/world 语义函数收口；本层不出现裸地址、不直接 include 符号头。

#include "features/world_teleport/feature.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/native/frame_task.h"
#include "core/native/qol_log.h"
#include "feature/ui/native_choice.h"
#include "features/world_teleport/rules.h"
#include "game/access/world.h"

namespace {

constexpr size_t kMapNameBufferSize = 256;
constexpr size_t kTeleportChoiceCount = 4;
constexpr size_t kChoiceCount = 5;
constexpr size_t kCloseChoiceIndex = kChoiceCount - 1;
constexpr int kChoiceDeltas[kTeleportChoiceCount] = {1, 10, -1, -10};
constexpr int kChoiceCosts[kTeleportChoiceCount] = {300, 3000, 300, 3000};
// 世界态（GAMESTATE 世界）值：与迁移前裸比较一致。
constexpr int kWorldState = 5;

// 阶段 2 真机对照开关：0=CallMapName 执行流内直接 push，1=下一逻辑帧 push。
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
// YesNo 第 7 参 param 语义=费用（UIPopupMsg 存全局槽，UINpcQuest_DrawEndPopup
// 在值 >0 时用 MONEY_DrawWithUnit 渲染价格栏），不能携带指针；待传送目标改由
// 此静态全局在确认/取消回调间传递。
ChoiceTarget g_pending_target{};

void teleport_confirmed(void* param);
void teleport_cancelled(void* param);

const char* map_name(int map_id) {
    const char* text = qol::game::world_map_name(map_id);
    return text != nullptr ? text : kUnknownMapName;
}

void teleport_on_select(int index, void* user);

void open_choice_panel() {
    if (!qol::game::world_ready()) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice symbols not resolved");
        return;
    }
    const int current_id = qol::game::world_current_map_id();
    const int record_count = qol::game::world_map_record_count();
    const int max_map_id = world_teleport::max_map_id_from_record_count(record_count);
    QOL_LOG_INFO(QolDomain::kUi,
                 "world teleport: map records count=%d max_map_id=%d current=%d",
                 record_count, max_map_id, current_id);
    std::array<std::array<char, kMapNameBufferSize>, kChoiceCount> choice_text{};
    std::array<const char*, kChoiceCount> items{};
    for (size_t index = 0; index < kTeleportChoiceCount; ++index) {
        const int delta = kChoiceDeltas[index];
        const int target_id = world_teleport::target_map_id(current_id, delta, max_map_id);
        const bool valid = target_id >= 0 &&
            (record_count <= 0 || target_id < record_count);
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
    std::snprintf(choice_text[kCloseChoiceIndex].data(),
                  choice_text[kCloseChoiceIndex].size(), "关闭");
    items[kCloseChoiceIndex] = choice_text[kCloseChoiceIndex].data();
    std::array<char, kMapNameBufferSize> title{};
    std::snprintf(title.data(), title.size(), "当前地图：%s(id:%d)",
                  map_name(current_id), current_id);
    const NativeChoiceSpec spec{items.data(), static_cast<int>(kChoiceCount), title.data(),
                                static_cast<int>(kCloseChoiceIndex), &teleport_on_select, nullptr};
    if (!native_choice_open(spec)) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice open failed");
        return;
    }
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: choice opened current=%d state=%d",
                 current_id, qol::game::world_choice_state_id());
}

bool delayed_open_choice(int64_t, void*) {
    open_choice_panel();
    return false;
}

bool delayed_open_confirmation(int64_t, void* param) {
    const auto* target = static_cast<const ChoiceTarget*>(param);
    if (target == nullptr || !qol::game::world_create_yesno_ready()) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: confirmation symbols not resolved");
        return false;
    }
    qol::game::world_create_yesno(g_confirm_text.data(),
                                  static_cast<uint32_t>(std::strlen(g_confirm_text.data())),
                                  reinterpret_cast<void*>(&teleport_confirmed),
                                  reinterpret_cast<void*>(&teleport_cancelled),
                                  reinterpret_cast<void*>(static_cast<intptr_t>(target->cost)));
    QOL_LOG_INFO(QolDomain::kUi,
                 "world teleport: confirmation opened target=%d cost=%d",
                 target->map_id, target->cost);
    return false;
}

void request_open_choice() {
#if WORLD_TELEPORT_OPEN_NEXT_FRAME
    if (frame_task_add(kFramePointLogicPre, delayed_open_choice, nullptr, 0, 1) == 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: delayed choice registration failed");
    }
#else
    open_choice_panel();
#endif
}

void show_message(const char* message) {
    if (!qol::game::world_show_banner(message)) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: instant message symbol not resolved");
        return;
    }
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: instant message shown text=%s", message);
}

bool delayed_transfer(int64_t, void* param) {
    const auto* target = static_cast<const ChoiceTarget*>(param);
    if (target == nullptr || !qol::game::world_money_ready() ||
        !qol::game::world_map_change_ready()) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: transfer symbols not resolved");
        return false;
    }
    const int direction = qol::game::world_active_player_direction();
    if (direction < 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: active player unavailable");
        return false;
    }
    const int64_t money = qol::game::world_money();
    if (money < target->cost || !qol::game::world_minus_money(static_cast<int64_t>(target->cost))) {
        show_message("金币不足，无法传送");
        QOL_LOG_INFO(QolDomain::kUi, "world teleport: insufficient money target=%d cost=%d money=%lld",
                     target->map_id, target->cost, static_cast<long long>(money));
        return false;
    }
    qol::game::world_request_map_change(target->map_id, 0, 0, direction);
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: transferred target=%d cost=%d direction=%d",
                 target->map_id, target->cost, direction);
    return false;
}

bool delayed_close_choice(int64_t, void* param) {
    if (!qol::game::world_close_popup_ready()) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice close symbols not resolved");
        return false;
    }
    // 确认回调运行在 YesNo 仍是栈顶时；确认回调中的第一条 close 只会关闭
    // YesNo。下一逻辑帧再排一条 close，才能关闭底层 UICHOICE，避免其
    // Scene_Process 在控件销毁后继续进入 ControlScroll_Process。
    qol::game::world_close_popup_process();
    qol::game::world_restore_hud_gate();
    if (frame_task_add(kFramePointLogicPre, delayed_transfer, param, 1, 1) == 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: transfer registration failed");
    }
    return false;
}

void teleport_confirmed(void*) {
    // YesNo 的 param 现在携带费用值（供价格栏渲染），目标统一读 g_pending_target。
    const ChoiceTarget* target = &g_pending_target;
    if (!target->valid || !qol::game::world_money_ready() ||
        !qol::game::world_map_change_ready()) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: confirm symbols not resolved");
        return;
    }
    // 先排队切图，再关闭 choice；切图必须等官方 Pop 在当前帧清理控件，
    // 否则下一帧的 ControlScroll_Process 会访问已失效的 choice 控件。
    if (frame_task_add(kFramePointLogicPre, delayed_close_choice,
                       const_cast<ChoiceTarget*>(target), 1, 1) == 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: transfer registration failed");
        return;
    }
    if (qol::game::world_close_popup_ready()) qol::game::world_close_popup_process();
    // 与官方 ButtonBackExe/data_op_panel_close 一致恢复 HUD gate。
    qol::game::world_restore_hud_gate();
}

void teleport_cancelled(void*) {
    // 官方流程会先关闭确认框；选择框已在打开确认框前关闭，因此下一逻辑帧
    // 重新 Push，而不是在 YesNo 仍位于栈顶时叠加 UICHOICE。
    if (frame_task_add(kFramePointLogicPre, delayed_open_choice, nullptr, 1, 1) == 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice reopen registration failed");
    }
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: confirmation cancelled");
}

void teleport_on_select(int index, void*) {
    if (!qol::game::world_create_yesno_ready()) {
        native_choice_close();
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: confirm symbols not resolved");
        return;
    }
    const ChoiceTarget& target = g_targets[static_cast<size_t>(index)];
    if (!target.valid) {
        show_message("目标地图无效，无法传送");
        native_choice_close();
        QOL_LOG_WARN(QolDomain::kUi,
                     "world teleport: invalid target index=%d map_id=%d",
                     index, target.map_id);
        return;
    }
    std::snprintf(g_confirm_text.data(), g_confirm_text.size(), "是否传送至%s？",
                  map_name(target.map_id));
    // UIPopupMsg 是全局绘制层；若底层 UICHOICE 仍在栈顶，弹窗状态虽已创建，
    // 实际画面仍会继续绘制 choice，导致用户看不到确认框。先按官方关闭流程
    // 清掉 choice，再在下一逻辑帧创建 YesNo；取消回调会重新打开 choice。
    if (!qol::game::world_close_popup_ready()) {
        native_choice_close();
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice close symbols not resolved");
        return;
    }
    native_choice_close();
    qol::game::world_restore_hud_gate();
    g_pending_target = target;
    if (frame_task_add(kFramePointLogicPre, delayed_open_confirmation,
                       &g_pending_target, 1, 1) == 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: confirmation registration failed");
        return;
    }
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: selected index=%d target=%d cost=%d",
                 index, target.map_id, target.cost);
}

}  // namespace

void world_teleport_call_map_name() {
    const int state = qol::game::world_state();
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: CallMapName intercepted state=%d", state);
    if (state != kWorldState) {
        QOL_LOG_WARN(QolDomain::kUi, "world teleport: ignored CallMapName outside world state=%d", state);
        return;
    }
    request_open_choice();
}

bool world_teleport_native_choice_install() {
    return native_choice_install_if_ready();
}
