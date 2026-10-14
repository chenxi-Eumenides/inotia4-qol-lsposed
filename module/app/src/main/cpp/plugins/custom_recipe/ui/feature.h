#pragma once

#include <cstddef>
#include <cstdint>

#include "../../../contracts/craft_commands.h"
#include "../../../contracts/craft_facts.h"
#include "../../../contracts/game_ports/craft_ui_write_port.h"
#include "../../../contracts/host_sdk.h"
#include "../../../contracts/item_facts.h"
#include "../result.h"

namespace qol::contracts {
struct PluginContext;
}

// 合成器界面子层：界面动作、文案与填入格状态。
//
// 分层（「界面归界面，结果归结果」）：
//   · 本层只管**界面**——填入了什么、选中哪一格、清空后恢复到哪个格、弹什么提示；
//     配方判定与扣料/产物决策调用父插件内部结果 API。
//   · 不 include 符号头、不出现裸偏移。
//
// 线程模型：全部函数只在游戏主线程（UI 回调 / wrapper）调用；只读写游戏内存，不加锁。

namespace qol::features::custom_recipe::ui {

enum class CraftPlaceDisposition : uint8_t {
    kStop,
    kAdvance,
    kAlreadyPlaced,
    kNotEnoughHeld,
    kWriteFailed,
};

constexpr CraftPlaceDisposition craft_place_disposition(result::PlaceOutcome outcome) {
    switch (outcome) {
        case result::PlaceOutcome::kPlaced:
            return CraftPlaceDisposition::kAdvance;
        case result::PlaceOutcome::kAlreadyPlaced:
            return CraftPlaceDisposition::kAlreadyPlaced;
        case result::PlaceOutcome::kNotEnoughHeld:
            return CraftPlaceDisposition::kNotEnoughHeld;
        case result::PlaceOutcome::kWriteFailed:
            return CraftPlaceDisposition::kWriteFailed;
        case result::PlaceOutcome::kNotMine:
        default:
            return CraftPlaceDisposition::kStop;
    }
}

using UIMixButtonInvenItemSelectExeFn = void (*)(void*);
using UIMixButtonMixingExeFn = void (*)(void*);
using MakeMixFn = int (*)(int32_t, void**);
using UIMixButtonMenuListExeFn = void (*)(void*);
using UIMixButtonRecipeExeFn = void (*)(void*);
using XTextCtrlSetTextControlFn = void (*)(void*, const char*, uint32_t, uint32_t, uint32_t,
                                           int32_t);
using ControlItemDrawFn = void (*)(void*);
using ItemDrawPortingFn = void (*)(void*, int32_t, int32_t, int32_t, int32_t);
using UIMixButtonRecipeDrawFn = void (*)(void*);
using UIMixDrawFn = void (*)();
using UIMixButtonMenuListDrawFn = void (*)(void*);
using UIMixResetStuffItemControlFn = void (*)();

constexpr size_t kStuffSlotCount = 3;

// QolLogLevel 的数值由 contracts/host_sdk.h 固定：debug/info/warn/error。
constexpr int32_t kLogDebug = 0;
constexpr int32_t kLogWarn = 2;

void set_context(const contracts::PluginContext& ctx);
void set_item_facts(const contracts::ItemFactsApi* facts);
void set_craft_facts(const contracts::CraftFactsApi* facts);
void set_write_port(const contracts::QolCraftUiWritePort* port);
const contracts::ItemFactsApi* item_facts();
const contracts::QolHostSdk* host_sdk();
void log_message(int32_t level, const char* format, ...);

uint32_t craft_mix_type();
int64_t craft_ui_type();
int64_t craft_selected_recipe_at(int64_t type);
void craft_set_selected_recipe_at(int64_t type, int64_t value);
int64_t craft_selected_stuff_index();
contracts::QolItemHandle craft_selected_inventory_item();
contracts::QolItemHandle craft_target_slot_item();
void craft_set_target_slot_item(contracts::QolItemHandle item);
void craft_set_cost_zero();
contracts::QolItemHandle craft_stuff_item(size_t index);
bool craft_stuff_slot_available(size_t index);
int craft_category_held_count(uint16_t category);
int craft_character_level();
void craft_read_stuff_categories(uint16_t out[kStuffSlotCount]);
void craft_write_stuff_need(size_t index, uint16_t category, uint16_t count);
void craft_set_stuff_item(size_t index, contracts::QolItemHandle item);
void craft_select_first_empty_stuff_slot();
void craft_reset_stuff_item_control();
void craft_refresh_inventory_items();
void craft_init_mixing_state();
void craft_set_ui_type(int64_t type);
void craft_play_sound(int16_t sound_id);
void craft_show_text(uint32_t text_id);
bool craft_show_yesno(uint32_t text_id, contracts::QolCraftPopupCallback ok,
                      contracts::QolCraftPopupCallback cancel);
int craft_panel_state();

// ---- 文案与音效：界面层的知识（出什么结果该说什么话）----
constexpr uint32_t kTextNoBagSpace = 5;             // 「背包空间不足，无法进行操作。」
constexpr uint32_t kTextOnlyJewel = 97;             // 「只有宝石道具才可以。」
constexpr uint32_t kTextMaxCrafted = 105;           // 「已经达到合成最大值，无法再次进行合成。」
constexpr uint32_t kTextInsufficientMaterial = 94;  // 「材料不足，无法进行合成。」
constexpr uint32_t kTextAlreadyPlaced = 99;         // 「该物品已经放入。」
constexpr uint32_t kTextCraftSuccess = 106;         // 「合成成功。」
constexpr uint32_t kTextConfirmCraft = 0x12;        // 「是否合成？」
constexpr int16_t kSoundCraftSuccess = 9;

// 模块配方描述文案（界面替换用）。必须与 plugins/custom_recipe/catalog.h 的
// kModuleRecipeDesc 逐字节一致（同一条产品文案；两处由注释 + host 断言保持同步）。
constexpr char kModuleRecipeDesc[] = "用3个材料合成";

// 真机验收用的有界诊断日志：每次进程最多 16 条，避免放料/合成高频刷屏。
bool log_budget_take();

// 原版 UIMix_StartMix 成功收尾序列（0xc0ab8-0xc09d4）：InitMixingState →
// ResetStuffItemControl → RefreshInvenItem → SOUNDSYSTEM_Play(9) → 弹 106。
void finish_three_slot_craft();

// 进入合成视图（type 1 且 state != 0）时选中第一个空格；其它情况不动。
void select_on_view_enter();

// 清空填入格 + 刷新背包网格（不含 InitMixingState、不含音效与提示）。
// 合成失败后的统一收尾，让玩家立刻重试。
void reset_stuff_and_refresh();

// Native draw callback pointer is consumed synchronously by the UI adapter; no control pointer is retained.
bool is_stuff_ctrl(void* ctrl);

class TextScopeGuard {
public:
    explicit TextScopeGuard(int32_t scope);
    ~TextScopeGuard();
    TextScopeGuard(const TextScopeGuard&) = delete;
    TextScopeGuard& operator=(const TextScopeGuard&) = delete;

private:
    const contracts::QolHostSdk* sdk_;
    int32_t previous_scope_;
    bool pushed_;
};

// 本地 TextScope 值；绘制范围 2/3/4 与 Host SDK scope 对齐，弹窗专用范围不进入 Host。
int32_t current_text_scope();

}  // namespace qol::features::custom_recipe::ui
