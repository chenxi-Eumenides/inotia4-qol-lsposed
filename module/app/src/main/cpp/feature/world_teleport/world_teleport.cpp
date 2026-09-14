#include "feature/world_teleport/world_teleport.h"

#include "core/native/frame_task.h"
#include "core/native/qol_log.h"
#include "feature/patch/native_inventory_hook.h"
#include "feature/world_teleport/world_teleport_rules.h"
#include "game_access.h"
#include "game_ptr_hook.h"
#include "game_state.h"
#include "game_symbols.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr size_t kPageSize = 0x1000;
constexpr int64_t kBranchRange = 0x08000000LL;
constexpr size_t kMapNameBufferSize = 256;
constexpr size_t kTeleportChoiceCount = 4;
constexpr size_t kChoiceCount = 5;
constexpr size_t kCloseChoiceIndex = kChoiceCount - 1;
constexpr int kChoiceDeltas[kTeleportChoiceCount] = {1, 10, -1, -10};
constexpr int kChoiceCosts[kTeleportChoiceCount] = {300, 3000, 300, 3000};

// 阶段 2 真机对照开关：0=CallMapName 执行流内直接 push，1=下一逻辑帧 push。
#ifndef WORLD_TELEPORT_OPEN_NEXT_FRAME
#define WORLD_TELEPORT_OPEN_NEXT_FRAME 0
#endif

struct ChoiceTarget {
    int map_id = 0;
    int cost = 0;
    bool valid = false;
};

std::mutex g_world_teleport_mtx;
PtrHook g_choice_button_hook;
bool g_installed = false;
bool g_choice_init_hook_installed = false;
bool g_pending_title = false;
UiChoiceInitFn g_choice_init_original = nullptr;
void* g_trampoline = nullptr;

std::array<ChoiceTarget, kTeleportChoiceCount> g_targets{};
std::array<std::array<char, kMapNameBufferSize>, kChoiceCount> g_choice_text{};
std::array<char, kMapNameBufferSize> g_confirm_text{};
std::array<char, kMapNameBufferSize> g_main_title{};
constexpr char kUnknownMapName[] = "未知地图";
// YesNo 第 7 参 param 语义=费用（UIPopupMsg 存全局槽，UINpcQuest_DrawEndPopup
// 在值 >0 时用 MONEY_DrawWithUnit 渲染价格栏），不能携带指针；待传送目标改由
// 此静态全局在确认/取消回调间传递。
ChoiceTarget g_pending_target{};

void teleport_confirmed(void* param);
void teleport_cancelled(void* param);

bool encode_b(uintptr_t from, uintptr_t to, uint32_t* out) {
    if (from == 0 || to == 0 || out == nullptr) return false;
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    constexpr int64_t kMin = -(int64_t{1} << 27);
    constexpr int64_t kMax = (int64_t{1} << 27) - 4;
    if ((delta & 0x3) != 0 || delta < kMin || delta > kMax) return false;
    *out = 0x14000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
    return true;
}

bool write_code_word(uintptr_t address, uint32_t word) {
    if (address == 0) return false;
    const uintptr_t page = address & ~(static_cast<uintptr_t>(kPageSize) - 1);
    if (mprotect(reinterpret_cast<void*>(page), kPageSize,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: mprotect failed addr=%p errno=%d",
                      reinterpret_cast<void*>(address), errno);
        return false;
    }
    *reinterpret_cast<uint32_t*>(address) = word;
    __builtin___clear_cache(reinterpret_cast<char*>(address),
                            reinterpret_cast<char*>(address + sizeof(uint32_t)));
    return true;
}

void* allocate_trampoline_near(uintptr_t target) {
#ifndef MAP_FIXED_NOREPLACE
    (void)target;
    return nullptr;
#else
    const uintptr_t base = target & ~(static_cast<uintptr_t>(kPageSize) - 1);
    for (const int64_t step : {int64_t{0x10000}, int64_t{0x1000}}) {
        for (int64_t distance = step; distance < kBranchRange; distance += step) {
            for (const int sign : {1, -1}) {
                const int64_t candidate = static_cast<int64_t>(base) + sign * distance;
                if (candidate <= 0) continue;
                void* region = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(candidate)),
                                    kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                if (region != MAP_FAILED) return region;
            }
        }
    }
    return nullptr;
#endif
}

const char* map_name(int map_id) {
    if (g_mapinfo_pdata_got == nullptr || g_mapinfo_record_size == nullptr ||
        g_mapinfo_record_count == nullptr || fn_memorytext_get_text == nullptr) {
        return kUnknownMapName;
    }
    auto* pdata_slot = reinterpret_cast<void**>(g_mapinfo_pdata_got);
    if (!game_memory_accessible(pdata_slot, sizeof(void*), 'r') || *pdata_slot == nullptr) {
        return kUnknownMapName;
    }
    auto* records = *reinterpret_cast<uint8_t**>(*pdata_slot);
    const uint8_t record_size = *reinterpret_cast<uint8_t*>(g_mapinfo_record_size);
    const uint16_t record_count = *reinterpret_cast<uint16_t*>(g_mapinfo_record_count);
    if (records == nullptr || record_size == 0 || map_id < 0 || map_id >= record_count) {
        return kUnknownMapName;
    }
    auto* record = records + static_cast<size_t>(map_id) * record_size;
    if (!game_memory_accessible(record, record_size, 'r')) return kUnknownMapName;
    const uint16_t text_id = *reinterpret_cast<uint16_t*>(record + MAPINFOBASE_RECORD_NAME_TEXT_ID);
    const char* text = fn_memorytext_get_text(text_id);
    if (text == nullptr || !game_memory_accessible(text, 1, 'r') || text[0] == '\0') {
        return kUnknownMapName;
    }
    return text;
}

int runtime_map_record_count() {
    if (g_mapinfo_record_count == nullptr ||
        !game_memory_accessible(g_mapinfo_record_count, sizeof(uint16_t), 'r')) {
        return 0;
    }
    return static_cast<int>(*reinterpret_cast<uint16_t*>(g_mapinfo_record_count));
}

int choice_state_id() {
    if (g_base == 0) return -1;
    auto* list_slot = reinterpret_cast<void**>(g_base + G_POPUP_STATE_LIST_GOT_VMA);
    if (!game_memory_accessible(list_slot, sizeof(void*), 'r') || *list_slot == nullptr) {
        return -1;
    }
    auto* list = reinterpret_cast<uint8_t*>(*list_slot);
    for (size_t index = 0; index < POPUP_STATE_COUNT; ++index) {
        auto* entry = list + index * POPUP_ENTRY_SIZE;
        const uintptr_t enter = *reinterpret_cast<uintptr_t*>(entry + POPUP_ENTRY_ENTER);
        if (enter == g_base + F_PANEL_CHOICE_ENTER) {
            return static_cast<int>(*reinterpret_cast<uint32_t*>(entry));
        }
    }
    return -1;
}

void open_choice_panel() {
    if (g_base == 0 || fn_popupstate_push == nullptr || g_uichoice_itemtext == nullptr ||
        g_uichoice_count == nullptr || g_uichoice_focus == nullptr ||
        !g_choice_init_hook_installed) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice symbols not resolved");
        return;
    }
    const int current_id = static_cast<int>(current_map_id());
    const int record_count = runtime_map_record_count();
    const int max_map_id = world_teleport::max_map_id_from_record_count(record_count);
    QOL_LOG_INFO(QolDomain::kUi,
                 "world teleport: map records count=%d max_map_id=%d current=%d",
                 record_count, max_map_id, current_id);
    auto** item_text = reinterpret_cast<char**>(g_uichoice_itemtext);
    for (size_t index = 0; index < kTeleportChoiceCount; ++index) {
        const int delta = kChoiceDeltas[index];
        const int target_id = world_teleport::target_map_id(current_id, delta, max_map_id);
        const bool valid = target_id >= 0 &&
            (record_count <= 0 || target_id < record_count);
        g_targets[index] = {target_id, kChoiceCosts[index], valid};
        const char* name = valid ? map_name(target_id) : "不可用";
        const char* sign = delta > 0 ? "+" : "-";
        if (valid) {
            std::snprintf(g_choice_text[index].data(), g_choice_text[index].size(),
                          "%s(id%s%d)", name, sign, delta > 0 ? delta : -delta);
        } else {
            std::snprintf(g_choice_text[index].data(), g_choice_text[index].size(),
                          "%s（地图数据不足）", name);
        }
        item_text[index] = g_choice_text[index].data();
    }
    std::snprintf(g_choice_text[kCloseChoiceIndex].data(),
                  g_choice_text[kCloseChoiceIndex].size(), "关闭");
    item_text[kCloseChoiceIndex] = g_choice_text[kCloseChoiceIndex].data();
    *reinterpret_cast<uint8_t*>(g_uichoice_count) = static_cast<uint8_t>(kChoiceCount);
    *reinterpret_cast<uint8_t*>(g_uichoice_focus) = 0;
    const int state_id = choice_state_id();
    if (state_id < 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice state not found");
        return;
    }
    std::snprintf(g_main_title.data(), g_main_title.size(), "当前地图：%s(id:%d)",
                  map_name(current_id), current_id);
    g_pending_title = true;
    if (fn_popupstate_push(static_cast<uint32_t>(state_id)) == 0) {
        g_pending_title = false;
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice push failed state=%d", state_id);
        return;
    }
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: choice opened current=%d state=%d",
                 current_id, state_id);
}

bool delayed_open_choice(int64_t, void*) {
    open_choice_panel();
    return false;
}

bool delayed_open_confirmation(int64_t, void* param) {
    const auto* target = static_cast<const ChoiceTarget*>(param);
    if (target == nullptr || fn_popup_create_yesno == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: confirmation symbols not resolved");
        return false;
    }
    fn_popup_create_yesno(g_confirm_text.data(),
                          static_cast<uint32_t>(std::strlen(g_confirm_text.data())),
                          0, 2, reinterpret_cast<void*>(&teleport_confirmed),
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

void world_teleport_call_map_name() {
    const int state = g_state == nullptr ? -1 :
        static_cast<int>(*reinterpret_cast<uint16_t*>(g_state));
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: CallMapName intercepted state=%d", state);
    if (state != 5) {
        QOL_LOG_WARN(QolDomain::kUi, "world teleport: ignored CallMapName outside world state=%d", state);
        return;
    }
    request_open_choice();
}

void show_message(const char* message) {
    if (fn_instantmsg_add == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: instant message symbol not resolved");
        return;
    }
    auto* text = const_cast<char*>(message);
    // 原版 UIPlay_CallMapName 的精确调用：Add(3, text, 0, 0, 5, 0x15, 0, 0)。
    // 这是非阻塞横幅，不创建 UIPopupMsg，避免余额不足时的失效按钮链表。
    fn_instantmsg_add(3, text, 0, 0, 5, 0x15, 0, 0);
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: instant message shown text=%s", message);
}

void* active_player() {
    if (g_player_active_got != nullptr &&
        game_memory_accessible(g_player_active_got, sizeof(void*), 'r')) {
        void* player_global = *reinterpret_cast<void**>(g_player_active_got);
        if (player_global != nullptr && game_memory_accessible(player_global, sizeof(void*), 'r')) {
            void* player = *reinterpret_cast<void**>(player_global);
            if (player != nullptr) return player;
        }
    }
    // 某些版本的 GOT 槽直接保存角色对象；已有 PLAYER_pActivePlayer 全局作为安全回退。
    if (g_player_active != nullptr &&
        game_memory_accessible(g_player_active, sizeof(void*), 'r')) {
        return *reinterpret_cast<void**>(g_player_active);
    }
    return nullptr;
}

bool delayed_transfer(int64_t, void* param) {
    const auto* target = static_cast<const ChoiceTarget*>(param);
    if (target == nullptr || fn_get_money == nullptr || fn_minus_money == nullptr ||
        fn_mapchange_set == nullptr || fn_gamestate_set_state == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: transfer symbols not resolved");
        return false;
    }
    void* player = active_player();
    if (player == nullptr || !game_memory_accessible(
            reinterpret_cast<uint8_t*>(player) + C_DIRECTION, sizeof(uint8_t), 'r')) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: active player unavailable");
        return false;
    }
    const int direction = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(player) + C_DIRECTION);
    const int64_t money = fn_get_money();
    if (money < target->cost || fn_minus_money(static_cast<int64_t>(target->cost)) == 0) {
        show_message("金币不足，无法传送");
        QOL_LOG_INFO(QolDomain::kUi, "world teleport: insufficient money target=%d cost=%d money=%lld",
                     target->map_id, target->cost, static_cast<long long>(money));
        return false;
    }
    fn_mapchange_set(target->map_id, 0, 0, direction);
    fn_gamestate_set_state(static_cast<int32_t>(GAMESTATE_MAP_CHANGE));
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: transferred target=%d cost=%d direction=%d",
                 target->map_id, target->cost, direction);
    return false;
}

bool delayed_close_choice(int64_t, void* param) {
    if (fn_ui_set_popup_process_info == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice close symbols not resolved");
        return false;
    }
    // 确认回调运行在 YesNo 仍是栈顶时；确认回调中的第一条 close 只会关闭
    // YesNo。下一逻辑帧再排一条 close，才能关闭底层 UICHOICE，避免其
    // Scene_Process 在控件销毁后继续进入 ControlScroll_Process。
    fn_ui_set_popup_process_info(3, 0);
    uint8_t** hud_gate = reinterpret_cast<uint8_t**>(g_base + G_HUD_GATE_GOT_VMA);
    if (hud_gate != nullptr && *hud_gate != nullptr) **hud_gate = 1;
    if (frame_task_add(kFramePointLogicPre, delayed_transfer, param, 1, 1) == 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: transfer registration failed");
    }
    return false;
}

void teleport_confirmed(void*) {
    // YesNo 的 param 现在携带费用值（供价格栏渲染），目标统一读 g_pending_target。
    const ChoiceTarget* target = &g_pending_target;
    if (!target->valid || fn_get_money == nullptr || fn_minus_money == nullptr ||
        fn_mapchange_set == nullptr || fn_gamestate_set_state == nullptr) {
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
    if (fn_ui_set_popup_process_info != nullptr) fn_ui_set_popup_process_info(3, 0);
    // 与官方 ButtonBackExe/data_op_panel_close 一致恢复 HUD gate。
    uint8_t** hud_gate = reinterpret_cast<uint8_t**>(g_base + G_HUD_GATE_GOT_VMA);
    if (hud_gate != nullptr && *hud_gate != nullptr) **hud_gate = 1;
}

void close_choice_panel() {
    g_pending_title = false;
    if (fn_ui_set_popup_process_info != nullptr) fn_ui_set_popup_process_info(3, 0);
    uint8_t** hud_gate = reinterpret_cast<uint8_t**>(g_base + G_HUD_GATE_GOT_VMA);
    if (hud_gate != nullptr && *hud_gate != nullptr) **hud_gate = 1;
}

void teleport_cancelled(void*) {
    // 官方流程会先关闭确认框；选择框已在打开确认框前关闭，因此下一逻辑帧
    // 重新 Push，而不是在 YesNo 仍位于栈顶时叠加 UICHOICE。
    if (frame_task_add(kFramePointLogicPre, delayed_open_choice, nullptr, 1, 1) == 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice reopen registration failed");
    }
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: confirmation cancelled");
}

void choice_button_execute(void* control) {
    if (control == nullptr || fn_control_object_get_cursor_index == nullptr ||
        g_uichoice_focus == nullptr || g_uichoice_control_got == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice selection symbols not resolved");
        return;
    }
    auto** control_slot = reinterpret_cast<void**>(g_uichoice_control_got);
    if (!game_memory_accessible(control_slot, sizeof(void*), 'r') || *control_slot == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice control unavailable");
        return;
    }
    const int index = fn_control_object_get_cursor_index(*control_slot);
    if (index < 0 || index >= static_cast<int>(kChoiceCount)) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: invalid choice index=%d", index);
        return;
    }
    *reinterpret_cast<uint8_t*>(g_uichoice_focus) = static_cast<uint8_t>(index);
    if (index == static_cast<int>(kCloseChoiceIndex)) {
        // 关闭项走原版 ButtonListExe 的完整清理链：除关闭 popup 外，还要让
        // EVTSYSTEM/UIChoice 自己完成 choice 状态清理，否则下一帧的
        // Scene_Process_POPUP_SC_CHOICE 会访问已失效控件。
        if (fn_uichoice_button_list_exe != nullptr) {
            fn_uichoice_button_list_exe(control);
        } else {
            close_choice_panel();
        }
        QOL_LOG_INFO(QolDomain::kUi, "world teleport: choice closed");
        return;
    }
    if (fn_popup_create_yesno == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: confirm symbols not resolved");
        return;
    }
    const ChoiceTarget& target = g_targets[static_cast<size_t>(index)];
    if (!target.valid) {
        show_message("目标地图无效，无法传送");
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
    if (fn_ui_set_popup_process_info == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice close symbols not resolved");
        return;
    }
    fn_ui_set_popup_process_info(3, 0);
    uint8_t** hud_gate = reinterpret_cast<uint8_t**>(g_base + G_HUD_GATE_GOT_VMA);
    if (hud_gate != nullptr && *hud_gate != nullptr) **hud_gate = 1;
    g_pending_target = target;
    if (frame_task_add(kFramePointLogicPre, delayed_open_confirmation,
                       &g_pending_target, 1, 1) == 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: confirmation registration failed");
        return;
    }
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: selected index=%d target=%d cost=%d",
                 index, target.map_id, target.cost);
}

void choice_init_wrapper(void* control) {
    if (g_choice_init_original != nullptr) g_choice_init_original(control);
    if (!g_pending_title) return;
    g_pending_title = false;
    if (g_uichoice_main_text == nullptr ||
        !game_memory_accessible(g_uichoice_main_text, sizeof(char*), 'w')) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice main title symbol unavailable");
        return;
    }
    *reinterpret_cast<char**>(g_uichoice_main_text) = g_main_title.data();
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: choice title set");
}

bool install_choice_init_hook() {
    if (g_choice_init_hook_installed) return true;
    if (fn_uichoice_init == nullptr || g_uichoice_main_text == nullptr) return false;
    const NativeHookFunType hook_func = native_hook_func();
    if (hook_func == nullptr) return false;
    void* backup = nullptr;
    const int rc = hook_func(reinterpret_cast<void*>(fn_uichoice_init),
                             reinterpret_cast<void*>(&choice_init_wrapper), &backup);
    if (rc != 0 || backup == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: UIChoice_Init hook failed rc=%d", rc);
        return false;
    }
    g_choice_init_original = reinterpret_cast<UiChoiceInitFn>(backup);
    g_choice_init_hook_installed = true;
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: UIChoice_Init hooked");
    return true;
}

bool install_choice_hook() {
    if (g_choice_button_hook.installed()) return true;
    if (g_uichoice_button_list_exe_got == nullptr || fn_uichoice_button_list_exe == nullptr) {
        return false;
    }
    void** slot = reinterpret_cast<void**>(g_uichoice_button_list_exe_got);
    if (!game_memory_accessible(slot, sizeof(void*), 'r')) return false;
    if (*slot != reinterpret_cast<void*>(fn_uichoice_button_list_exe)) {
        QOL_LOG_ERROR(QolDomain::kUi,
                      "world teleport: choice ExecuteProc slot mismatch slot=%p got=%p expected=%p",
                      slot, *slot, reinterpret_cast<void*>(fn_uichoice_button_list_exe));
        return false;
    }
    const uintptr_t page = reinterpret_cast<uintptr_t>(slot) & ~(kPageSize - 1);
    if (mprotect(reinterpret_cast<void*>(page), kPageSize, PROT_READ | PROT_WRITE) != 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice hook mprotect failed errno=%d", errno);
        return false;
    }
    if (!g_choice_button_hook.install_typed(slot, &choice_button_execute)) return false;
    QOL_LOG_INFO(QolDomain::kUi, "world teleport: choice ExecuteProc hooked slot=%p", slot);
    return true;
}

bool install_entry_patch(bool* supported) {
    *supported = false;
    if (g_trampoline != nullptr) {
        *supported = true;
        return true;
    }
    const uintptr_t patch_addr = g_base + F_UI_PLAY_CALL_MAP_NAME_VMA +
                                 F_UI_PLAY_CALL_MAP_NAME_PATCH_OFF;
    const uintptr_t return_addr = g_base + F_UI_PLAY_CALL_MAP_NAME_VMA +
                                  F_UI_PLAY_CALL_MAP_NAME_EPILOGUE_OFF;
    uint32_t legacy_word = 0;
    if (!encode_b(patch_addr, g_base + F_UI_PLAY_CALL_MAP_NAME_LEGACY_TARGET_VMA, &legacy_word)) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: legacy branch encoding failed");
        return false;
    }
    const uint32_t original_word = 0xd2800020u; // mov x0, #1
    const uint32_t current_word = *reinterpret_cast<const uint32_t*>(patch_addr);
    if (current_word != original_word && current_word != legacy_word) {
        QOL_LOG_WARN(QolDomain::kUi,
                     "world teleport: unsupported entry instruction=0x%08x expected mov=0x%08x legacy=0x%08x",
                     current_word, original_word, legacy_word);
        return true;
    }

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = static_cast<long>(kPageSize);
    const size_t length = static_cast<size_t>(page);
    void* region = allocate_trampoline_near(patch_addr);
    if (region == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi,
                      "world teleport: near trampoline mmap failed patch=%p errno=%d",
                      reinterpret_cast<void*>(patch_addr), errno);
        return false;
    }
    auto* code = reinterpret_cast<uint8_t*>(region);
    uint32_t return_branch = 0;
    if (!encode_b(reinterpret_cast<uintptr_t>(code) + 12, return_addr, &return_branch)) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: trampoline return branch out of range");
        munmap(region, length);
        return false;
    }
    uint32_t patch_branch = 0;
    if (!encode_b(patch_addr, reinterpret_cast<uintptr_t>(code), &patch_branch)) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: entry branch out of range");
        munmap(region, length);
        return false;
    }
    const uint32_t trampoline_code[] = {
        0x58000090u, // ldr x16, #16
        0xd63f0200u, // blr x16
        0xd2800020u, // mov x0, #1
        return_branch,
    };
    std::memcpy(code, trampoline_code, sizeof(trampoline_code));
    *reinterpret_cast<uintptr_t*>(code + 16) = reinterpret_cast<uintptr_t>(&world_teleport_call_map_name);
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code + 16 + sizeof(uintptr_t)));
    if (!write_code_word(patch_addr, patch_branch)) {
        munmap(region, length);
        return false;
    }
    g_trampoline = region;
    *supported = true;
    QOL_LOG_INFO(QolDomain::kUi,
                 "world teleport: entry patched addr=%p old=0x%08x branch=0x%08x trampoline=%p",
                 reinterpret_cast<void*>(patch_addr), current_word, patch_branch, region);
    return true;
}

}  // namespace

bool world_teleport_install_if_ready() {
    std::lock_guard<std::mutex> lock(g_world_teleport_mtx);
    if (g_installed) return true;
    if (!bridge_ready() || g_base == 0) return false;
    bool supported = false;
    if (!install_entry_patch(&supported)) return false;
    if (!supported) return true;
    if (!install_choice_init_hook()) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice init hook install failed");
        return false;
    }
    if (!install_choice_hook()) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: choice hook install failed");
        return false;
    }
    g_installed = true;
    return true;
}
