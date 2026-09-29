#include "game/access/save.h"

#include "game/symbols/game_symbols.h"
#include "game/access/game_access.h"
#include "game/state/game_state.h"

namespace qol::game {

bool save_slot_meta_deps_ready() {
    return fn_save_get_save_slot != nullptr && fn_saveslot_get_hero != nullptr &&
           fn_save_destroy_save_slot != nullptr && fn_save_create_save_slot != nullptr;
}

void save_slots_refresh_if_stale() {
    ::save_slots_refresh_if_stale();
}

void save_slots_mark_stale() {
    ::save_slots_mark_stale();
}

void* save_slot_struct(int32_t slot) {
    return fn_save_get_save_slot != nullptr ? fn_save_get_save_slot(slot) : nullptr;
}

bool save_slot_exists(const void* slot_struct) {
    if (slot_struct == nullptr) return false;
    return reinterpret_cast<const uint8_t*>(slot_struct)[SAVESLOT_EXISTS] != 0;
}

int save_slot_map_id(const void* slot_struct) {
    if (slot_struct == nullptr) return -1;
    return *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(slot_struct) +
                                              SAVESLOT_MAP_ID);
}

int save_slot_hero_index(const void* slot_struct) {
    if (slot_struct == nullptr) return -1;
    return static_cast<int8_t>(
        reinterpret_cast<const uint8_t*>(slot_struct)[SAVESLOT_HERO_INDEX]);
}

void* save_slot_hero(void* slot_struct) {
    return fn_saveslot_get_hero != nullptr ? fn_saveslot_get_hero(slot_struct) : nullptr;
}

int save_hero_level(const void* hero) {
    if (hero == nullptr) return -1;
    return static_cast<int8_t>(reinterpret_cast<const uint8_t*>(hero)[C_LEVEL]);
}

int save_hero_class(const void* hero) {
    if (hero == nullptr) return -1;
    return static_cast<int8_t>(reinterpret_cast<const uint8_t*>(hero)[C_CLASS]);
}

int save_current_slot() { return ::current_save_slot(); }

int save_is_ok() {
    return fn_save_is_ok == nullptr ? -1 : (fn_save_is_ok() != 0 ? 1 : 0);
}

bool save_load_deps_ready() {
    return fn_save_load_data != nullptr && fn_mem_free != nullptr;
}

int save_load_data(int32_t slot, void** out_buf, int* out_len) {
    if (fn_save_load_data == nullptr) return 0;
    return fn_save_load_data(slot, out_buf, out_len);
}

void save_mem_free(void* buf) {
    if (fn_mem_free != nullptr && buf != nullptr) fn_mem_free(buf);
}

const char* save_hub_get_key() {
    return fn_hub_save_get_key != nullptr ? fn_hub_save_get_key() : nullptr;
}

bool save_encrypt_available() {
    return fn_encrypt_process2 != nullptr;
}

int save_encrypt_process2(void* buf, int len, int mode, const char* key) {
    if (fn_encrypt_process2 == nullptr) return 0;
    return fn_encrypt_process2(buf, len, mode, key);
}

bool save_slot_delete_hook_target(void** out_slot, uintptr_t* out_expected) {
    if (g_base == 0 || out_slot == nullptr || out_expected == nullptr) return false;
    const uintptr_t offset = fn_resolve("F_SAVESLOT_DELETE_VMA", F_SAVESLOT_DELETE_VMA);
    if (offset == 0) return false;  // 符号未解析：fn_resolve 返回 0
    *out_slot = reinterpret_cast<void*>(g_base + G_SAVESLOT_DELETE_GOT_VMA);
    *out_expected = g_base + offset;
    return true;
}

}  // namespace qol::game

// ---------------------------------------------------------------------------
// T2l-s-write：存档写原语（迁移自 api/native 存档写实现 save/enter_slot(_direct)/
// create_slot(_direct)）。转场/能力现取经 contracts/runtime_hooks.h 过渡注入面。
// ---------------------------------------------------------------------------
#include "contracts/module_save_port.h"
#include "contracts/panels.h"
#include "contracts/runtime_hooks.h"
#include "game/adapter_ports/lifecycle_participant_facade.h"
#include "game/access/save_preflight.h"
#include "game/ops/game_ops_common.h"

#include <cstring>

namespace qol::game {
namespace {

constexpr int32_t kSaveTransitionHeavyTimeoutMs = 15000;

// UI 占用守卫：面板插件经 `ui_panels` 能力提供占用原因（文案由各 provider 持有，
// 逐字保持 "ui occupied: backup panel" / "ui occupied: settings panel"）。
// 每次现取能力、不缓存：core.ui 被停用/撤销后 require 返回 nullptr，守卫自动失效不阻塞。
const char* savebackup_ui_block_reason() {
    const auto* api = static_cast<const contracts::UiPanelsApi*>(
        contracts::qol_runtime_hooks().capability_require(
            contracts::kUiPanelsCapabilityId,
            static_cast<uint16_t>(contracts::kUiPanelsVersionMajor),
            static_cast<uint16_t>(contracts::kUiPanelsVersionMinor)));
    if (api == nullptr || api->block_reason == nullptr) return nullptr;
    return api->block_reason();
}

SaveWriteResult pack_result(const std::string& body) {
    SaveWriteResult out{};
    const size_t size = body.size() < sizeof(out.text) ? body.size() : sizeof(out.text);
    if (size > 0) std::memcpy(out.text, body.data(), size);
    out.size = static_cast<uint32_t>(size);
    return out;
}

// 转场结果渲染：executed=true 时 out 为完整 op JSON；false 时 out 为 busy/超时纯文本
//（迁移前调用点 `return op_err(result.c_str())`，空串同样进 error 字段，逐字保持）。
std::string render_transition(const QolResult& result, const char* body, uint32_t size) {
    const std::string text(body, size);
    if (!contracts::succeeded(result)) return op_err(text.c_str());
    return text;
}

// 预检（backlog P0②）：仅主菜单（STATE==4）可取判决——非主菜单调 CreateSaveSlot 会让
// SAVE_LoadInformation 覆盖世界态全局（版本/时长等，docs/reference/game/save.md §3），故其余
// 状态返回 unknown/not in main menu，不刷新、只读当前槽结构字节。判决由槽区重载产生
// （同一次主菜单停留内只重载一次，见 game/access/game_access.h 刷新策略注释），调用方不得再额外读档。
std::string save_preflight_for_enter(int32_t slot) {
    if (g_state == nullptr) return op_err("libgame not ready");
    if (fn_save_get_save_slot == nullptr || fn_save_destroy_save_slot == nullptr ||
        fn_save_create_save_slot == nullptr || fn_saveslot_get_hero == nullptr)
        return op_err("symbol not resolved");
    if (slot < 0 || slot > 2) return op_err("bad slot");
    uint16_t st = *reinterpret_cast<uint16_t*>(g_state);
    if (st != 4) {
        std::string e = "not in main menu (state=" + std::to_string(st) + ")";
        return op_err(e.c_str());
    }
    if (g_popup_on != nullptr && *reinterpret_cast<uint8_t*>(g_popup_on))
        return op_err("ui occupied: dialog_popup");
    if (const char* ui_block = savebackup_ui_block_reason()) {
        return op_err(ui_block);
    }
    // 判决来自槽区重载（内部 SAVE_CreateSaveSlot 逐槽 SAVESLOT_Initialize + SAVE_LoadSaveSlot，
    // 与游戏存档面板同源）；本次主菜单停留内已重载过则直接复用，不重复重载。
    // 此处不得再对目标槽单独调 SAVE_LoadSaveSlot：那是重复读档，会额外累积游戏侧对象，
    // 使本次及后续读档在角色阶段失败（原因见 game/access/game_access.h 刷新策略注释）。
    save_slots_refresh_if_stale();
    void* ss = fn_save_get_save_slot(slot);
    if (ss == nullptr) return op_err("bad slot");
    uint8_t* p = reinterpret_cast<uint8_t*>(ss);
    uint8_t slot_state = p[2];
    uint8_t slot_err = p[3];
    int map_id = *reinterpret_cast<uint16_t*>(p);
    int hero_level = 0;
    int hero_index = -1;
    std::string detail;
    if (slot_state == 2) {
        void* hero = fn_saveslot_get_hero(ss);
        hero_index = *reinterpret_cast<int8_t*>(p + SAVESLOT_HERO_INDEX);
        if (hero == nullptr) {
            void* raw_hero = nullptr;
            if (hero_index >= 0 && hero_index < 3) {
                raw_hero = *reinterpret_cast<void**>(p + SAVESLOT_HERO_PTRS + hero_index * sizeof(void*));
            }
            if (raw_hero == nullptr) {
                uint8_t* player_indices = *reinterpret_cast<uint8_t**>(g_base + G_PLAYER_INDICES_GOT_VMA);
                std::string detail = "slot state is loaded but raw hero pointer is null";
                if (player_indices != nullptr && g_main_merc_slot != nullptr) {
                    detail += "; player_indices=" + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[0])))
                        + "," + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[1])))
                        + "," + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[2])))
                        + ", main_merc_slot=" + std::to_string(static_cast<int>(static_cast<int8_t>(*reinterpret_cast<uint8_t*>(g_main_merc_slot))));
                }
                return save_preflight_semantic_error_json(slot, "character", 7, detail.c_str());
            }
            return save_preflight_semantic_error_json(
                slot, "character", 7,
                "slot raw hero pointer is non-null but SAVESLOT_GetHero returned null");
        }
        hero_level = static_cast<int8_t>(*reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(hero) + C_LEVEL));
        uint8_t* player_indices = *reinterpret_cast<uint8_t**>(g_base + G_PLAYER_INDICES_GOT_VMA);
        if (player_indices != nullptr && g_main_merc_slot != nullptr) {
            detail = "player_indices=" + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[0])))
                + "," + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[1])))
                + "," + std::to_string(static_cast<int>(static_cast<int8_t>(player_indices[2])))
                + ", main_merc_slot=" + std::to_string(static_cast<int>(static_cast<int8_t>(*reinterpret_cast<uint8_t*>(g_main_merc_slot))));
        }
    }
    if (save_preflight_classify(slot_state, slot_err) != SavePreflightVerdict::kValid) {
        return save_preflight_error_json(slot, slot_state, slot_err);
    }
    return save_preflight_json(slot, slot_state, slot_err, map_id, hero_level, hero_index,
                               detail.empty() ? nullptr : detail.c_str());
}

std::string enter_slot_locked(int32_t slot) {
    if (g_state == nullptr) return op_err("libgame not ready");
    if (const char* ui_block = savebackup_ui_block_reason()) return op_err(ui_block);
    uint16_t st = *reinterpret_cast<uint16_t*>(g_state);
    if (st == 5) return op_err("already in game");
    // 前置检查（v0.5.7）：仅主菜单（STATE==4）可进档。loading/切换态（STATE=0xFFFF）下
    // GAME_Initialize 未完成，GAME_StartResumeGame→ASSYSTEM_Initialize 空指针崩溃
    // （真机 tombstone 实测：ASNODE_Initialize+4 fault addr 0xe）
    if (st != 4) {
        std::string e = "not in main menu (state=" + std::to_string(st) + ")";
        return op_err(e.c_str());
    }
    if (fn_save_get_save_slot == nullptr || fn_ui_set_popup_process_info == nullptr ||
        fn_game_start_resume_game == nullptr || fn_saveslot_get_hero == nullptr)
        return op_err("symbol not resolved");
    if (slot < 0 || slot > 2) return op_err("bad slot");
    // 槽区刷新由预检内部完成（save_slots_refresh_if_stale）；此处不得再刷新，多一次重载就多
    // 一批未释放的角色对象（见 game/access/game_access.h 刷新策略注释）。
    std::string preflight = save_preflight_for_enter(slot);
    if (preflight.find("\"verdict\":\"valid\"") == std::string::npos) return preflight;
    void* slot_struct = fn_save_get_save_slot(slot);
    if (slot_struct == nullptr || fn_saveslot_get_hero(slot_struct) == nullptr) return op_err("slot corrupt");
    // 存档/UI 生命周期参与者分派：无可用参与者等同旧“能力缺失”，跳过准备放行。
    if (!qol::game::lifecycle_participant_prepare_save_slot_load()) {
        return op_err("extension bag cleanup failed; enter slot blocked");
    }
    fn_ui_set_popup_process_info(4, 0);
    uint8_t** flag_ptr = reinterpret_cast<uint8_t**>(g_base + G_GAME_RESUME_FLAG_GOT_VMA);
    if (*flag_ptr != nullptr) **flag_ptr = 0;
    contracts::qol_runtime_hooks().save_enter_mark_pending();  // 进入存档回调：标记读档发起
    int r = fn_game_start_resume_game(slot);
    if (!r) return op_err("enter slot failed");

    // v0.4.49：进档后清理残留教学暂停——obj170=6（药水教学）是持久状态，
    // 回主菜单（GAMESTATE_SetState(4)）与 GAME_StartResumeGame 均不清理，
    // API 进档后若仍为 6 会残留 tutorial_pause 卡住移动。手动进档无此问题
    // （用户从正常世界态回主菜单时 obj170 已非 6）。进档即复位教学。
    if (tutorial_state() == 6) tutorial_cancel();
    return op_ok();
}

// v0.4.64：创建新角色存档（复刻官方 SaveSlot_GoToNewGame + SelectCharacter_ButtonStartExe 链，
// frida 全流程监听实证，见 docs/systems/save.md §10）
std::string create_slot_locked(int32_t slot, int32_t class_idx) {
    if (g_state == nullptr) return op_err("libgame not ready");
    if (const char* ui_block = savebackup_ui_block_reason()) return op_err(ui_block);
    uint16_t st = *reinterpret_cast<uint16_t*>(g_state);
    if (st == 5) return op_err("already in game");
    if (st != 4) {
        std::string e = "not in main menu (state=" + std::to_string(st) + ")";
        return op_err(e.c_str());
    }
    if (slot < 0 || slot > 2) return op_err("bad slot");
    if (class_idx < 0 || class_idx > 5) return op_err("bad class");
    if (g_base == 0) return op_err("libgame not ready");
    // 同 enter_slot_locked：无可用参与者等同旧“能力缺失”，跳过准备放行。
    if (!qol::game::lifecycle_participant_prepare_save_slot_load()) {
        return op_err("extension bag cleanup failed; create slot blocked");
    }
    if (fn_save_create_save_slot == nullptr || fn_save_destroy_save_slot == nullptr ||
        fn_game_exit_save_slot_select_char == nullptr ||
        fn_select_character_start_game == nullptr || fn_tutorial_start == nullptr ||
        fn_save_get_save_file_name == nullptr || fn_cs_fs_remove == nullptr)
        return op_err("symbol not resolved");
    // 槽区初始化（重载三槽存档到内存，确保槽位状态可用；先释放上一批角色对象）
    save_slots_reload();
    // 删除目标槽旧存档文件（SaveSlot_GoToNewGame 官方链：SAVE_GetSaveFileName + CS_fsRemove）
    char fname[128] = {0};
    fn_save_get_save_file_name(slot, fname);
    if (fname[0] != '\0') fn_cs_fs_remove(fname, 1);
    // 槽文件已改动：槽区结构不再代表磁盘状态，下一次读取必须重载。
    save_slots_mark_stale();
    // 当前槽位（SaveSlot_GoToNewGame：*[0x2f4000+0xd20] = slot）
    uint8_t** cur_slot = reinterpret_cast<uint8_t**>(g_base + G_CURRENT_SLOT_GOT_VMA);
    if (cur_slot == nullptr || *cur_slot == nullptr) return op_err("save slot state not ready");
    **cur_slot = static_cast<uint8_t>(slot);
    // 新建标志（SaveSlot_GoToNewGame：*[0x2f6000+0x8] = 1；STATE_EnterGame 检测后走 GAME_StartNewGame）
    uint8_t** newgame_flag = reinterpret_cast<uint8_t**>(g_base + G_GAME_RESUME_FLAG_GOT_VMA);
    if (newgame_flag == nullptr || *newgame_flag == nullptr) return op_err("newgame flag not ready");
    **newgame_flag = 1;
    // 选中职业（SelectCharacter_StartGame 读取源 [0x308080+0x8]）
    *reinterpret_cast<uint32_t*>(g_base + G_SELECTED_CLASS_VMA) = static_cast<uint32_t>(class_idx);
    // 进入选角环境（GAME_Initialize + MAP_Load(6) + MAINMENU_CreateSelectCharList）
    fn_game_exit_save_slot_select_char();
    // 选角确认开始（*[0x2f5000+0xa00] = class_idx + STATE_Set(5) + UI_SetPopupProcessInfo(4,0)）
    contracts::qol_runtime_hooks().save_enter_mark_pending();  // 进入存档回调：标记新档发起
    fn_select_character_start_game();
    // 新档教学初始化（SelectCharacter_ButtonStartExe：StartGame 后 TutorialStart）
    fn_tutorial_start();
    // 状态机驱动：STATE_NextStartProcess → STATE_EnterGame → GAME_StartNewGame → 剧情 → 初始营地
    return op_ok();
}

void copy_body(const std::string& body, char* out, uint32_t capacity, uint32_t* out_len) {
    const size_t size = body.size() < capacity ? body.size() : capacity;
    if (size > 0) std::memcpy(out, body.data(), size);
    *out_len = static_cast<uint32_t>(size);
}

void enter_slot_body(void* ctx, char* out, uint32_t capacity, uint32_t* out_len) {
    copy_body(enter_slot_locked(*static_cast<const int32_t*>(ctx)), out, capacity, out_len);
}

struct CreateSlotCtx {
    int32_t slot;
    int32_t class_idx;
};

void create_slot_body(void* ctx, char* out, uint32_t capacity, uint32_t* out_len) {
    const auto* c = static_cast<const CreateSlotCtx*>(ctx);
    copy_body(create_slot_locked(c->slot, c->class_idx), out, capacity, out_len);
}

}  // namespace

SaveWriteResult save_game() {
    if (!game_in_world()) return pack_result(op_err("not in game"));
    // 原版保存只发生在世界完全就绪后（保存点/存档菜单，角色链表与主控均已加载）。
    // 进档后世界异步加载期（约 1-3 秒）角色/主控指针尚未就绪，此时调 SAVE_Save 会在
    // CHAR_GetStatusPoint 解引用空角色指针 SIGSEGV（真机 tombstone 实测，见 backlog
    // P0「世界加载完成前保存的崩溃防护」）——对齐原版前置：主控未就绪即拒绝保存。
    if (member_or_null(0) == nullptr) return pack_result(op_err("world not ready"));
    if (const char* ui_block = savebackup_ui_block_reason()) return pack_result(op_err(ui_block));
    if (fn_save == nullptr) return pack_result(op_err("symbol not resolved"));
    return pack_result(module_save_game() ? op_ok() : op_err("save failed"));
}

SaveWriteResult enter_slot(int32_t slot) {
    // 读档链（GAME_StartResumeGame/SAVE 系列）必须在游戏主线程的逻辑帧点执行。
    char body[1024]{};
    uint32_t size = 0;
    const QolResult result = contracts::qol_runtime_hooks().transition_run(
        &enter_slot_body, &slot, kSaveTransitionHeavyTimeoutMs, body, sizeof(body), &size);
    return pack_result(render_transition(result, body, size));
}

SaveWriteResult create_slot(int32_t slot, int32_t class_idx) {
    // 新档链（GAME_Initialize/SelectCharacter/STATE_Set）必须在游戏主线程的逻辑帧点执行。
    char body[1024]{};
    uint32_t size = 0;
    CreateSlotCtx ctx{slot, class_idx};
    const QolResult result = contracts::qol_runtime_hooks().transition_run(
        &create_slot_body, &ctx, kSaveTransitionHeavyTimeoutMs, body, sizeof(body), &size);
    return pack_result(render_transition(result, body, size));
}

SaveWriteResult enter_slot_direct(int32_t slot) {
    // owner 命令已在 kFramePointLogicPre：同转换体内联执行，共用单飞门，不阻塞自己。
    char body[1024]{};
    uint32_t size = 0;
    const QolResult result = contracts::qol_runtime_hooks().transition_run_inline(
        &enter_slot_body, &slot, body, sizeof(body), &size);
    return pack_result(render_transition(result, body, size));
}

SaveWriteResult create_slot_direct(int32_t slot, int32_t class_idx) {
    char body[1024]{};
    uint32_t size = 0;
    CreateSlotCtx ctx{slot, class_idx};
    const QolResult result = contracts::qol_runtime_hooks().transition_run_inline(
        &create_slot_body, &ctx, body, sizeof(body), &size);
    return pack_result(render_transition(result, body, size));
}

}  // namespace qol::game

// ---------------------------------------------------------------------------
// T2l-s-read：存档槽读面（迁自 api/native/game_save.cpp，逐字）。落 game 层原因：消费者
// game_system.cpp 属 legacy/system，不能 include host（allows 仅 legacy/save→host）。
// ---------------------------------------------------------------------------
namespace qol::game {

std::string current_save_slot_json() {
    return "{\"current_save_slot\":" + std::to_string(current_save_slot()) + "}";
}

std::string save_slots_json() {
    if (fn_save_get_save_slot == nullptr || fn_saveslot_get_hero == nullptr ||
        fn_save_destroy_save_slot == nullptr || fn_save_create_save_slot == nullptr)
        return op_err("symbol not resolved");
    // SAVE_CreateSaveSlot 会重载三槽并覆盖角色相关全局；只允许在主菜单刷新，且同一次主菜单停留内
    // 只重载一次（反复重载会累积游戏侧对象，见 game/access/game_access.h 刷新策略注释）。
    // world/启动过渡阶段查询 save_slots 必须只读现有槽结构，不能因 GET /system/info 破坏当前游戏。
    save_slots_refresh_if_stale();
    std::string s = "{\"slots\":[";
    for (int i = 0; i < 3; ++i) {
        if (i > 0) s += ",";
        void* slot = fn_save_get_save_slot(i);
        uint8_t b2 = slot ? *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(slot) + SAVESLOT_EXISTS) : 0;
        int8_t hero_idx = slot ? *reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(slot) + SAVESLOT_HERO_INDEX) : -1;
        bool exists = (b2 != 0);
        s += "{\"slot\":" + std::to_string(i) + ",\"exists\":" + (exists ? "true" : "false");
        if (exists) {
            void* hero = fn_saveslot_get_hero(slot);
            int level = hero ? static_cast<int8_t>(*reinterpret_cast<int8_t*>(reinterpret_cast<uint8_t*>(hero) + C_LEVEL)) : 0;
            s += ",\"hero_level\":" + std::to_string(level) + ",\"hero_index\":" + std::to_string(hero_idx);
        }
        s += "}";
    }
    s += "]}";
    return s;
}

}  // namespace qol::game

// ---------------------------------------------------------------------------
// T2n-3：save 发起点原语（符号 VMA 常量与调用偏移收口在 game 层）。
// ---------------------------------------------------------------------------
namespace qol::game {

SaveSiteRef save_enter_load_site() {
    return {F_SAVESLOT_SLOT_BUTTON_EXE_VMA, F_SAVESLOT_SLOT_BUTTON_EXE_RESUME_CALL_OFF};
}

SaveSiteRef save_enter_new_site() {
    return {F_SELECTCHAR_BUTTON_START_EXE_VMA,
            F_SELECTCHAR_BUTTON_START_EXE_STARTGAME_CALL_OFF};
}

SaveSiteRef save_exit_site() {
    return {F_GAMESTATE_SET_STATE_VMA, F_GAMESTATE_SET_STATE_GAME_EXIT_CALL_OFF};
}

}  // namespace qol::game
