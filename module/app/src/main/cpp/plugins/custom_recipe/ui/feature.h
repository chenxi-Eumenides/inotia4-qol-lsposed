#pragma once

#include <cstddef>
#include <cstdint>

#include "../../../contracts/craft_commands.h"
#include "../../../contracts/craft_facts.h"
#include "../../../contracts/game_ports/craft_ui_write_port.h"
#include "../../../contracts/game_ports/custom_recipe_write_port.h"
#include "../../../contracts/host_sdk.h"
#include "../../../contracts/item_facts.h"
#include "../model/result.h"

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
constexpr int32_t kLogInfo = 1;
constexpr int32_t kLogWarn = 2;

void set_context(const contracts::PluginContext& ctx);
void set_item_facts(const contracts::ItemFactsApi* facts);
void set_craft_facts(const contracts::CraftFactsApi* facts);
void set_write_port(const contracts::QolCraftUiWritePort* port);
void set_recipe_write_port(const contracts::QolCustomRecipeWritePort* port);
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
// 与 craft_target_slot_item 同源，但保留读取结果码：目标槽读取失败时调用方
// 必须能区分「槽是空的」与「句柄/映射不可用」，否则门禁只会给出同一句提示。
QolResult craft_read_target_slot_item(contracts::QolItemHandle* out_item);
void craft_set_target_slot_item(contracts::QolItemHandle item);
void craft_set_cost_zero();
// UI_Desc_SetOff（经 custom_recipe write port description_set_off）。原版放料
// （UIMix_ButtonInvenItemSelectExe）入口无条件调用该函数；扩展袋物品跳过原生放料时
// 由本函数补齐同一效果。
void craft_desc_set_off();

// 三同类宝石三合一委托原版 UIMix_StartMix（经 craft_ui_write_port v1.2 delegate_start_mix）。
// 返回 false = 端口未就绪/配方非法（未进入原版 StartMix，材料未扣）。
bool craft_delegate_start_mix(uint32_t mix_type);
// 扩展袋升阶：以指定费用（0）委托原版 UIMix_StartMix，绕过 Monster 改版合成按钮 type 3 分支。
// 返回 false = 端口未就绪/未进入 StartMix，材料未扣。
bool craft_delegate_start_mix_with_cost(uint32_t mix_type, int64_t cost);
contracts::QolItemHandle craft_stuff_item(size_t index);
bool craft_stuff_slot_available(size_t index);
int craft_category_held_count(uint16_t category);
int craft_character_level();
// 读取三格类别（0 = 真空槽）。任一槽读取/映射失败经 QolResult 明确返回：
// 不得把「读取失败」或「非空对象但类别不可知」伪装成空槽 0。
QolResult craft_read_stuff_categories(uint16_t out[kStuffSlotCount]);
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
constexpr uint32_t kTextOnlyJewel = 97;             // 「只有宝石或装备才可以。」
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
