#pragma once

#include <cstddef>
#include <cstdint>

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"

namespace qol::contracts {
struct PluginContext;
}

// 合成器界面层 feature（C2 Step 3）：界面动作、文案与填入格控件缓存。
//
// 分层（「界面归界面，结果归结果」）：
//   · 本层只管**界面**——填入了什么、选中哪一格、清空后恢复到哪个格、弹什么提示；
//     配方判定与扣料/产物决策一律经能力契约 `craft_result` 问结果层（provider=plugins/custom_recipe）。
//   · 本层不反向依赖任何 feature；不 include 符号头、不出现裸偏移。
//
// 线程模型：全部函数只在游戏主线程（UI 回调 / wrapper）调用；只读写游戏内存，不加锁。

namespace qol::features::craft_ui {

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
const contracts::QolHostSdk* host_sdk();
const contracts::QolGameHost* game_host();
void log_message(int32_t level, const char* format, ...);
bool config_enabled(const char* key);

uint32_t craft_mix_type();
int64_t craft_ui_type();
int64_t craft_selected_recipe_at(int64_t type);
void craft_set_selected_recipe_at(int64_t type, int64_t value);
int64_t craft_selected_stuff_index();
void* craft_selected_inventory_item();
void* craft_target_slot_item();
void craft_set_target_slot_item(void* item);
void* craft_stuff_item(size_t index);
void* craft_stuff_slot_control(size_t index);
int craft_category_held_count(uint16_t category);
int craft_character_level();
void craft_read_stuff_categories(uint16_t out[kStuffSlotCount]);
void craft_write_stuff_need(size_t index, uint16_t category, uint16_t count);
void craft_set_stuff_item(size_t index, void* item);
void craft_select_first_empty_stuff_slot();
void craft_reset_stuff_item_control();
void craft_refresh_inventory_items();
void craft_init_mixing_state();
void craft_set_ui_type(int64_t type);
void craft_play_sound(int16_t sound_id);
void craft_show_text(uint32_t text_id);
bool craft_show_yesno(uint32_t text_id, void* ok, void* cancel);
void craft_description_set_off();
int craft_panel_state();
void craft_select_stuff_slot(int64_t index);

// 配置仍开启但结果能力缺失时，hook 仍处于模块上下文，必须拒绝原版回落；配置关闭时
// 旧面板撤销窗口仍允许透传原版路径。
constexpr bool craft_result_unavailable_is_fail_closed(bool module_gate_enabled) {
    return module_gate_enabled;
}

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

// 填入格控件指针缓存（在放料/合成 hook 内刷新——那些时机面板必然打开）。绘制层只做**指针
// 比较**、不再解引用 `[+0xc8]`：面板关闭后缓存可能悬空，但仅比较不会崩，最坏是漏/误隐藏一次数量。
void refresh_stuff_ctrls();
bool is_stuff_ctrl(void* ctrl);

class TextScopeGuard {
public:
    explicit TextScopeGuard(int32_t scope);
    ~TextScopeGuard();
    TextScopeGuard(const TextScopeGuard&) = delete;
    TextScopeGuard& operator=(const TextScopeGuard&) = delete;

private:
    const contracts::QolHostSdk* sdk_;
    bool pushed_;
};

}  // namespace qol::features::craft_ui
