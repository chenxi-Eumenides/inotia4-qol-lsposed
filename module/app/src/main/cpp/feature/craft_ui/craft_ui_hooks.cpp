#include "feature/craft_ui/craft_ui_hooks.h"

#include "core/native/qol_log.h"
#include "feature/craft_ui/craft_ui.h"
#include "feature/custom_recipe/custom_recipe_api.h"
#include "feature/custom_recipe/custom_recipe_catalog.h"
#include "feature/custom_recipe/custom_recipe_table.h"
#include "feature/patch/native_inventory_hook.h"
#include "feature/ui/module_text.h"
#include "game_access.h"
#include "game_symbols.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

// ===========================================================================
// 合成器界面层：全部 UIMix hook 的唯一挂载处
// ===========================================================================
//
// 分层（用户裁定 2026-09-16：界面归界面，结果归结果）：
//   本文件 = **界面**。判断「这三格能合成什么」一律问 feature/custom_recipe 的
//   custom_recipe_api.h，并按它回传的枚举决定弹什么文案、要不要清空填入格。
//   反过来，配方层不认识界面：它只回结果枚举，一行弹窗/选中代码都没有。
//
// 挂钩清单（同一 UIMix 函数只挂一次，不再有「两家各挂一套、靠调用地址巧合串起来」的隐式依赖）：
//   · UIMix_ButtonInvenItemSelectExe  放料
//   · UIMix_ButtonMixingExe           合成按钮
//   · UIMix_ButtonMenuListExe         页签（进入视图）
//   · UIMix_ButtonRecipeExe           配方点击
//   · MIXSYSTEM_MakeItem              产物
//   · X_TEXTCTRL_SetTextControl       描述文本落点（装饰性）
//   · ControlItem_Draw / ITEM_DrawPorting  3 格填入格不显示整堆数量（装饰性）
//   · UIMix_ButtonRecipeDraw / UIMix_Draw / UIMix_ButtonMenuListDraw  模块文案窗口（装饰性）
//   · UIMix_ResetStuffItemControl     清空填入格 → 补选中第一个空格（装饰性，但**不随开关门控**：
//     清空可能来自模块 3 格配方，也可能来自原版成功链，两者都需要它）

namespace {

// ---- 文案与音效：界面层的知识（出什么结果该说什么话）----
constexpr uint32_t kTextNoBagSpace = 5;              // 「背包空间不足，无法进行操作。」
constexpr uint32_t kTextOnlyJewel = 97;              // 「只有宝石道具才可以。」
constexpr uint32_t kTextMaxCrafted = 105;            // 「已经达到合成最大值，无法再次进行合成。」
constexpr uint32_t kTextInsufficientMaterial = 94;   // 「材料不足，无法进行合成。」
constexpr uint32_t kTextAlreadyPlaced = 99;          // 「该物品已经放入。」
constexpr uint32_t kTextCraftSuccess = 106;          // 「合成成功。」
constexpr uint32_t kTextConfirmCraft = 0x12;         // 「是否合成？」
constexpr int16_t kSoundCraftSuccess = 9;

constexpr QolDomain kDomain = QolDomain::kCustomRecipe;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_attempted{false};
std::atomic<int> g_verify_log_budget{16};

// ---- hook 跳板（LSPosed 写入）----
UIMixButtonInvenItemSelectExeFn g_backup_place = nullptr;
UIMixButtonMixingExeFn g_backup_mixing = nullptr;
MakeMixFn g_backup_make_item = nullptr;
UIMixButtonMenuListExeFn g_backup_menu = nullptr;
UIMixButtonRecipeExeFn g_backup_recipe = nullptr;
XTextCtrlSetTextControlFn g_backup_set_desc_text = nullptr;
ControlItemDrawFn g_backup_control_item_draw = nullptr;
ItemDrawPortingFn g_backup_item_draw_porting = nullptr;
UIMixButtonRecipeDrawFn g_backup_button_recipe_draw = nullptr;
UIMixDrawFn g_backup_uimix_draw = nullptr;
UIMixButtonMenuListDrawFn g_backup_menu_list_draw = nullptr;
UIMixResetStuffItemControlFn g_backup_reset_stuff = nullptr;

bool log_budget_take() {
    return qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0;
}

// ---- 3 格填入格「不显示整堆数量」：格子语义 = 1 个单位 ----
// 反汇编定案：`ControlItem_Draw@0xaaedc` 画图标时调 `ITEM_DrawPorting@0x10644c(item,x,y,type,
// show_count)`，且**硬编码 show_count=1**（`0xaaf24 mov w4,#1`）；`ITEM_DrawPorting` 仅在该标志非 0
// **且**数量 >1 时画数量（`0x106550 cbnz w22` → `0x106628`；`0x106630 cmp w0,#1; b.le` 跳过）。
// 于是：在 `ControlItem_Draw` 层识别「正在画的是本模式的填入格」，在 `ITEM_DrawPorting` 层把
// show_count 压成 0 —— **只影响本模式的填入格**；背包等其它界面画的是同一个堆对象，数量照常显示。
// 为什么不动调用点：`0xaaf28` 已被扩展背包的 draw gate BL patch 占用（F_ITEM_DRAW_PORTING_CALL_VMA）。
// 绘制路径：**零日志**（R4）。
std::atomic<bool> g_drawing_three_slot_slot{false};

// 填入格控件指针缓存（在放料/合成 hook 内刷新——那些时机面板必然打开）。绘制层只做**指针比较**、
// 不再解引用 `[+0xc8]`：面板关闭后缓存可能悬空，但仅比较不会崩，最坏是漏/误隐藏一次数量。
void* g_three_slot_ctrl[craft_ui::kStuffSlotCount] = {nullptr, nullptr, nullptr};

void refresh_three_slot_ctrls() {
    for (size_t i = 0; i < craft_ui::kStuffSlotCount; ++i) {
        g_three_slot_ctrl[i] = craft_ui::stuff_slot_control(i);
    }
}

// ---- 界面动作 ----

// 原版 UIMix_StartMix 成功收尾序列（0xc0ab8-0xc09d4）：InitMixingState →
// ResetStuffItemControl → RefreshInvenItem → SOUNDSYSTEM_Play(9) → 弹 106。
void finish_three_slot_craft() {
    craft_ui::init_mixing_state();
    craft_ui::reset_stuff_item_control();
    craft_ui::refresh_inven_items();
    craft_ui::play_sound(kSoundCraftSuccess);
    craft_ui::show_text(kTextCraftSuccess);
}

// 进入合成视图（type 1 且 state != 0）时选中第一个空格；其它情况不动。
void select_on_view_enter() {
    if (g_uimix == nullptr || fn_uimix_get_type == nullptr) return;
    if (fn_uimix_get_type() != 1) return;
    const uint8_t state =
        *reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(g_uimix) + UIMIX_SLOT_STATE);
    if (state == 0) return;
    craft_ui::select_first_empty_stuff_slot();
}

// 3 格合成的 YesNo 确认回调（经 x3 传入，void() 形态；原版该槽位是 UIMix_StartMix@0xc0870）。
// 上下文只能用模块自有全局（x6/param 会被原版当钱数渲染）。
// 结果由配方层回传，界面按枚举收尾（弹什么、清不清空都在这一层决定）。
void on_confirm_three_slot() {
    switch (custom_recipe::three_slot_execute()) {
        case custom_recipe::CraftOutcome::kOk:
            finish_three_slot_craft();
            return;
        case custom_recipe::CraftOutcome::kStaleSlots:
            // 确认框停留期间第 1 格被换掉：不消耗、不产出 → 提示 + 清空重来。
            craft_ui::show_text(kTextInsufficientMaterial);
            craft_ui::reset_stuff_and_refresh();
            return;
        case custom_recipe::CraftOutcome::kNotEnoughHeld:
            // 扣料前库存复核失败：不消耗、不产出（保留已填格，便于玩家补材料后重试）。
            craft_ui::show_text(kTextInsufficientMaterial);
            return;
        case custom_recipe::CraftOutcome::kNoBagSpace:
            craft_ui::show_text(kTextNoBagSpace);
            craft_ui::reset_stuff_and_refresh();
            return;
    }
}

// ---------------------------------------------------------------------------
// wrapper：放料（UIMix_ButtonInvenItemSelectExe）
// ---------------------------------------------------------------------------
void place_execute(void* ctrl) {
    if (g_backup_place == nullptr) return;
    // 3 格隐式配方（type 1「合成」入口）：任意物品可填入 ⇒ 原版三道校验不可用，整段自实现。
    if (custom_recipe::three_slot_mode_active()) {
        refresh_three_slot_ctrls();  // 面板必然打开：刷新填入格控件缓存（绘制层只比较、不解引用）
        const int64_t index = craft_ui::selected_stuff_index();
        switch (custom_recipe::three_slot_place(index)) {
            case custom_recipe::PlaceOutcome::kAlreadyPlaced:
                craft_ui::show_text(kTextAlreadyPlaced);  // 99：不可堆叠物品不能重复放置
                return;
            case custom_recipe::PlaceOutcome::kNotEnoughHeld:
                // 94：持有总数不足，无法再占一格。放料失败**不清空**已填格（本次放置被拒，
                // 格子里没有新内容）。
                craft_ui::show_text(kTextInsufficientMaterial);
                return;
            case custom_recipe::PlaceOutcome::kPlaced:
                craft_ui::select_first_empty_stuff_slot();  // 放料后自动选中下一空格
                return;
            case custom_recipe::PlaceOutcome::kNotMine:
                return;  // 不可能：上面已判定处于 3 格模式
        }
        return;
    }
    // def 非空即代表「当前选中的是模块注入的 kJewelTierUp 配方」，与总开关无关
    //（停用后旧面板仍可能持有它，必须能识别以 fail-closed）。
    const custom_recipe::Def* def = custom_recipe::def_for_mix_type(craft_ui::mix_type());
    if (def == nullptr) {
        g_backup_place(ctrl);
        return;
    }
    if (!custom_recipe::enabled()) {
        // 停用后残留的旧面板仍可能指向注入配方：原版会照常写入目标槽并算费用，
        // 且合成时会走原版通用路径白扣材料 —— 故转调后立即撤销放置。
        g_backup_place(ctrl);
        craft_ui::set_target_slot_item(nullptr);
        return;
    }
    if (!custom_recipe::custom_recipe_table_ensure()) {
        g_backup_place(ctrl);
        return;
    }
    g_backup_place(ctrl);  // 原版：CheckMixture（注入 mixType 恒返回 0）→ SetItem → 写费用

    void* item = craft_ui::target_slot_item();
    if (item == nullptr) return;  // 本次未放入任何物品
    if (!custom_recipe::is_jewel_item(item)) {
        craft_ui::set_target_slot_item(nullptr);  // 撤销放置
        craft_ui::show_text(kTextOnlyJewel);
        return;
    }
    // 宝石：按宝石档位 + 角色等级写入材料需求数（下一帧绘制即显示），再强制费用 0
    //（覆盖原函数 CAL_Calculate 结果，显示与实扣同时为 0）。
    custom_recipe::apply_material_count_for(def, item);
    *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_COST) = 0;
    if (log_budget_take()) {
        QOL_LOG_DEBUG(kDomain, "place accepted jewel mixType=%u cost=0", craft_ui::mix_type());
    }
}

// ---------------------------------------------------------------------------
// wrapper：合成按钮（UIMix_ButtonMixingExe）
// ---------------------------------------------------------------------------
void craft_execute(void* ctrl) {
    if (g_backup_mixing == nullptr) return;
    // 3 格隐式配方：合成前先按 3 格内容查表。命中 → 弹原版 YesNo、回调走模块产出流程；
    // 未命中 → 弹 94 + 清空重来。两条路径都**不**转调原函数 ⇒ 既不进原版 StartMix、
    // 也不产生任何费用/扣料（本模式无费用）。
    if (custom_recipe::three_slot_mode_active()) {
        refresh_three_slot_ctrls();
        switch (custom_recipe::three_slot_prepare()) {
            case custom_recipe::PrepareOutcome::kNoMatch:
                if (log_budget_take()) {
                    uint16_t cats[craft_ui::kStuffSlotCount] = {0, 0, 0};
                    craft_ui::read_stuff_categories(cats);
                    QOL_LOG_DEBUG(kDomain, "three slot miss slots=%u,%u,%u", cats[0], cats[1],
                                  cats[2]);
                }
                craft_ui::show_text(kTextInsufficientMaterial);
                craft_ui::reset_stuff_and_refresh();
                return;
            case custom_recipe::PrepareOutcome::kConfirmPending:
                if (fn_popup_create_yesno_from_textdata == nullptr) {
                    QOL_LOG_WARN(kDomain, "three slot confirm skipped reason=no_popup");
                    return;
                }
                fn_popup_create_yesno_from_textdata(
                    kTextConfirmCraft, 0, 0, reinterpret_cast<void*>(&on_confirm_three_slot),
                    nullptr, nullptr);
                return;
            case custom_recipe::PrepareOutcome::kNotMine:
                return;  // 不可能：上面已判定处于 3 格模式
        }
        return;
    }
    const custom_recipe::Def* def = custom_recipe::def_for_mix_type(craft_ui::mix_type());
    if (def == nullptr) {
        g_backup_mixing(ctrl);
        return;
    }
    if (!custom_recipe::enabled() || !custom_recipe::custom_recipe_table_ensure()) {
        // fail-closed：不允许经残留面板对该 mixType 发起合成。原版会走通用
        // CreatePerfectItem 路径：产物无意义、材料与费用照扣。宁可无响应也不白扣。
        QOL_LOG_WARN(kDomain, "craft blocked reason=disabled mixType=%u", craft_ui::mix_type());
        return;
    }
    switch (custom_recipe::craft_gate()) {
        case custom_recipe::CraftGate::kNotJewel:
            craft_ui::show_text(kTextOnlyJewel);
            return;
        case custom_recipe::CraftGate::kAlreadyGold:
            craft_ui::show_text(kTextMaxCrafted);
            return;
        case custom_recipe::CraftGate::kPass:
            break;
    }
    g_backup_mixing(ctrl);  // 费用 0 时原版自动跳过金币校验并弹确认框 → StartMix
}

// ---------------------------------------------------------------------------
// wrapper：产物（MIXSYSTEM_MakeItem）
// ---------------------------------------------------------------------------
int make_item_execute(int32_t mix_type, void** out_item) {
    const int result = custom_recipe::make_item(mix_type, out_item);
    if (result >= 0) return result;
    return (g_backup_make_item != nullptr) ? g_backup_make_item(mix_type, out_item) : 1;
}

// ---------------------------------------------------------------------------
// wrapper：页签（UIMix_ButtonMenuListExe）
// ---------------------------------------------------------------------------
// 点任意页签时，本函数随即执行 SetType → MIXSYSTEM_CreateRecipeList → 建配方按钮。
// 注入必须早于建按钮，故在此处（转调之前）确保注入完成，使首次点开宝石强化页即能看到注入配方。
void menu_execute(void* ctrl) {
    if (custom_recipe::enabled()) {
        custom_recipe::custom_recipe_table_ensure();
    }
    if (g_backup_menu != nullptr) {
        g_backup_menu(ctrl);
    }
    select_on_view_enter();
}

// ---------------------------------------------------------------------------
// wrapper：配方点击（UIMix_ButtonRecipeExe）
// ---------------------------------------------------------------------------
// 原函数把「本组数组下标」写进 [+0x100+当前type*8]、把 recipeList[下标] 写进 [+0x48]，
// 并按当前 type 走原版流程。转调**后**若 [+0x48] 命中模块配方且其 Form 借用的 type ≠ 当前
// type（宝石强化页 group 3 上：「合成」= type 1 同型不动作；「宝石强化」= type 3 需切型），
// 则 SetType → InitMixingState → ResetStuffItemControl 按形式重建合成状态。
// 无论是否同型，点击都会把自定义数组的下标留在该 type 的已选槽里 → 转调前快照、换型后恢复，
// 防污染原版该 type 的已选槽（后续切页签时按旧槽读 recipeList 会错选/越界）。同型不做事。
void recipe_execute(void* ctrl) {
    if (g_backup_recipe == nullptr) return;
    const int64_t clicked_type = craft_ui::ui_type();
    const int64_t prev_selected = craft_ui::selected_recipe_at(clicked_type);
    g_backup_recipe(ctrl);

    if (custom_recipe::def_for_mix_type(craft_ui::mix_type()) != nullptr) {
        const int64_t form_type = custom_recipe::form_ui_type_for_mix_type(craft_ui::mix_type());
        if (form_type >= 0 && form_type != craft_ui::ui_type()) {
            craft_ui::set_ui_type(form_type);
            craft_ui::init_mixing_state();
            craft_ui::reset_stuff_item_control();
            craft_ui::set_selected_recipe_at(clicked_type, prev_selected);  // 恢复点击前的已选槽
            if (log_budget_take()) {
                QOL_LOG_DEBUG(kDomain, "custom recipe picked mixType=%u type=%lld",
                              craft_ui::mix_type(), static_cast<long long>(form_type));
            }
        }
    }
    select_on_view_enter();
}

// ---------------------------------------------------------------------------
// wrapper：描述文案替换（X_TEXTCTRL_SetTextControl）
// ---------------------------------------------------------------------------
// 门控（最终版，按**内容**而非指针）。真机日志定案（见设计册 §7.28）：
//   这段描述的写入者是 `UIMix_Draw@0xc1654` 内部（ra=0xc20a4），**逐帧重建**；
//   它用的文本控件与文本缓冲都是面板**堆对象**，**不是**静态全局 0x302d98/0x303dc0
//   （那是另一个 UIDesc 实例，属于装备详情面板）—— 按指针比对永远不成立。
// 因此改为在**共用的文本落点**上拦截：命中时先把描述缓冲内容换成模块文案，再转调原函数 ——
// 宽度/滚动/点亮全部由原调用点用自己的实参完成，模块既不重放刷新链、也不写任何游戏文本数据。
// 本 hook 可能处于 UI 事件路径，不得有任何级别日志（R4；诊断走 debug + 有界预算）。
void set_desc_text_execute(void* ctrl, const char* text, uint32_t width, uint32_t f1, uint32_t f2,
                           int32_t f3) {
    if (g_backup_set_desc_text == nullptr) return;
    if (custom_recipe::desc_should_replace(text)) {
        constexpr size_t kLen = sizeof(custom_recipe::kModuleRecipeDesc) - 1;
        auto* buf = const_cast<char*>(text);
        for (size_t n = 0; n < kLen; ++n) buf[n] = custom_recipe::kModuleRecipeDesc[n];
        buf[kLen] = '\0';
        if (log_budget_take()) {
            QOL_LOG_DEBUG(kDomain, "desc replaced mixType=%u", craft_ui::mix_type());
        }
    }
    g_backup_set_desc_text(ctrl, text, width, f1, f2, f3);
}

// 填入格数量隐藏：绘制层只做指针比较（见上文注释）。
void control_item_draw_execute(void* ctrl) {
    if (g_backup_control_item_draw == nullptr) return;
    bool is_three_slot = false;
    if (ctrl != nullptr && g_uimix != nullptr && custom_recipe::three_slot_mode_active()) {
        for (size_t i = 0; i < craft_ui::kStuffSlotCount; ++i) {
            if (g_three_slot_ctrl[i] == ctrl) {
                is_three_slot = true;
                break;
            }
        }
    }
    if (!is_three_slot) {
        g_backup_control_item_draw(ctrl);
        return;
    }
    const bool prev = g_drawing_three_slot_slot.exchange(true, std::memory_order_acq_rel);
    g_backup_control_item_draw(ctrl);
    g_drawing_three_slot_slot.store(prev, std::memory_order_release);
}

void item_draw_porting_execute(void* item, int32_t x, int32_t y, int32_t type, int32_t show_count) {
    if (g_backup_item_draw_porting == nullptr) return;
    if (g_drawing_three_slot_slot.load(std::memory_order_acquire)) show_count = 0;
    g_backup_item_draw_porting(item, x, y, type, show_count);
}

// ---------------------------------------------------------------------------
// wrapper：模块配方文案（「宝石升阶」）的三个绘制窗口
// ---------------------------------------------------------------------------
// 同一个 wordId（35291）在 UIMix 面板里被三处复用 ——
//   ① 配方按钮      UIMix_ButtonRecipeDraw    → 要显示「宝石升阶」（Scope::kUimixRecipeButton）
//   ② 面板标题      UIMix_Draw 内的「当前配方名」→ 也要显示「宝石升阶」
//   ③ 宝石强化页页签 UIMix_ButtonMenuListDraw  → **必须保持原文「宝石强化」**
// ③ 在 ② 内部被嵌套调用，靠 TextScopeGuard 的保存/恢复语义把作用域压成 kUimixPageTab。
void button_recipe_draw_execute(void* ctrl) {
    module_text::TextScopeGuard scope(module_text::Scope::kUimixRecipeButton);
    if (g_backup_button_recipe_draw != nullptr) g_backup_button_recipe_draw(ctrl);
}

void uimix_draw_execute() {
    module_text::TextScopeGuard scope(module_text::Scope::kUimixPanelTitle);
    if (g_backup_uimix_draw != nullptr) g_backup_uimix_draw();
}

void menu_list_draw_execute(void* ctrl) {
    module_text::TextScopeGuard scope(module_text::Scope::kUimixPageTab);
    if (g_backup_menu_list_draw != nullptr) g_backup_menu_list_draw(ctrl);
}

// ---------------------------------------------------------------------------
// wrapper：清空填入格（UIMix_ResetStuffItemControl）
// ---------------------------------------------------------------------------
// 覆盖该函数的**所有**调用者：原版合成成功链、模块 3 格配方的失败清空、配方换型重建……
// 清空会把「当前选中填入格」打回 -1（未选中），玩家点背包物品没有落点、只能先手动点一次格子。
// 故统一在清空后补「选中第一个空格」。
// **不随开关门控**：清空可能来自模块 3 格配方（它有自己的开关），不该被别的开关左右。
void reset_stuff_execute() {
    if (g_backup_reset_stuff == nullptr) return;
    g_backup_reset_stuff();
    craft_ui::select_first_empty_stuff_slot();
}

}  // namespace

bool craft_ui_hooks_installed() { return g_installed.load(std::memory_order_acquire); }

bool craft_ui_install_if_ready() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    if (!bridge_ready()) return false;
    NativeHookFunType hook = native_hook_func();
    if (hook == nullptr) return false;

    bool expected = false;
    if (!g_attempted.compare_exchange_strong(expected, true)) {
        return g_installed.load(std::memory_order_acquire);
    }

    const uintptr_t place = g_base + fn_resolve("F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA",
                                                F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA);
    const uintptr_t mixing = g_base + fn_resolve("F_UIMIX_BUTTON_MIXING_EXE_VMA",
                                                 F_UIMIX_BUTTON_MIXING_EXE_VMA);
    const uintptr_t make_item = g_base + fn_resolve("F_MAKE_MIX_VMA", F_MAKE_MIX_VMA);
    const uintptr_t menu =
        g_base + fn_resolve("F_UIMIX_BUTTON_MENU_LIST_EXE_VMA", F_UIMIX_BUTTON_MENU_LIST_EXE_VMA);
    const uintptr_t recipe_exe =
        g_base + fn_resolve("F_UIMIX_BUTTON_RECIPE_EXE_VMA", F_UIMIX_BUTTON_RECIPE_EXE_VMA);

    if (!craft_ui::install_one(hook, place, reinterpret_cast<void*>(&place_execute),
                              reinterpret_cast<void**>(&g_backup_place),
                              "UIMix_ButtonInvenItemSelectExe") ||
        !craft_ui::install_one(hook, mixing, reinterpret_cast<void*>(&craft_execute),
                              reinterpret_cast<void**>(&g_backup_mixing), "UIMix_ButtonMixingExe") ||
        !craft_ui::install_one(hook, make_item, reinterpret_cast<void*>(&make_item_execute),
                              reinterpret_cast<void**>(&g_backup_make_item), "MIXSYSTEM_MakeItem") ||
        !craft_ui::install_one(hook, menu, reinterpret_cast<void*>(&menu_execute),
                              reinterpret_cast<void**>(&g_backup_menu), "UIMix_ButtonMenuListExe") ||
        !craft_ui::install_one(hook, recipe_exe, reinterpret_cast<void*>(&recipe_execute),
                              reinterpret_cast<void**>(&g_backup_recipe), "UIMix_ButtonRecipeExe")) {
        return false;
    }

    // 以下均为**装饰性**能力：单独挂载、失败只告警，**绝不拖垮核心链**。
    // 教训：`UIDesc_MakeItem@0xb36a0` 已被 attribute_range 功能 hook，对同一地址二次挂载
    // `native_hook_func` 返回 -1；早期把它串进主链导致 5 个核心 hook 全部未安装（真机实证）
    const uintptr_t set_desc_text =
        g_base + fn_resolve("F_XTEXTCTRL_SET_TEXT_CONTROL_VMA", F_XTEXTCTRL_SET_TEXT_CONTROL_VMA);
    if (!craft_ui::install_one(hook, set_desc_text, reinterpret_cast<void*>(&set_desc_text_execute),
                              reinterpret_cast<void**>(&g_backup_set_desc_text),
                              "X_TEXTCTRL_SetTextControl")) {
        QOL_LOG_WARN(kDomain, "desc text hook skipped reason=install_failed");
    }

    const uintptr_t control_item_draw =
        g_base + fn_resolve("F_CONTROL_ITEM_DRAW_VMA", F_CONTROL_ITEM_DRAW_VMA);
    const uintptr_t item_draw_porting =
        g_base + fn_resolve("F_ITEM_DRAW_PORTING_VMA", F_ITEM_DRAW_PORTING_VMA);
    if (!craft_ui::install_one(hook, control_item_draw,
                              reinterpret_cast<void*>(&control_item_draw_execute),
                              reinterpret_cast<void**>(&g_backup_control_item_draw),
                              "ControlItem_Draw") ||
        !craft_ui::install_one(hook, item_draw_porting,
                              reinterpret_cast<void*>(&item_draw_porting_execute),
                              reinterpret_cast<void**>(&g_backup_item_draw_porting),
                              "ITEM_DrawPorting")) {
        QOL_LOG_WARN(kDomain, "slot count hide hook skipped reason=install_failed");
    }

    const uintptr_t button_recipe_draw =
        g_base + fn_resolve("F_UIMIX_BUTTON_RECIPE_DRAW_VMA", F_UIMIX_BUTTON_RECIPE_DRAW_VMA);
    const uintptr_t uimix_draw = g_base + fn_resolve("F_UIMIX_DRAW_VMA", F_UIMIX_DRAW_VMA);
    const uintptr_t menu_list_draw =
        g_base + fn_resolve("F_UIMIX_BUTTON_MENU_LIST_DRAW_VMA", F_UIMIX_BUTTON_MENU_LIST_DRAW_VMA);
    if (!craft_ui::install_one(hook, button_recipe_draw,
                              reinterpret_cast<void*>(&button_recipe_draw_execute),
                              reinterpret_cast<void**>(&g_backup_button_recipe_draw),
                              "UIMix_ButtonRecipeDraw")) {
        QOL_LOG_WARN(kDomain, "recipe label hook skipped reason=install_failed");
    }
    if (!craft_ui::install_one(hook, uimix_draw, reinterpret_cast<void*>(&uimix_draw_execute),
                              reinterpret_cast<void**>(&g_backup_uimix_draw), "UIMix_Draw")) {
        QOL_LOG_WARN(kDomain, "recipe title hook skipped reason=install_failed");
    }
    if (!craft_ui::install_one(hook, menu_list_draw,
                              reinterpret_cast<void*>(&menu_list_draw_execute),
                              reinterpret_cast<void**>(&g_backup_menu_list_draw),
                              "UIMix_ButtonMenuListDraw")) {
        QOL_LOG_WARN(kDomain, "page tab hook skipped reason=install_failed");
    }

    // 清空填入格 → 补选中空格。函数入口 inline hook ⇒ 覆盖所有调用者。
    if (fn_uimix_reset_stuff_item_control != nullptr) {
        if (!craft_ui::install_one(hook,
                                  reinterpret_cast<uintptr_t>(fn_uimix_reset_stuff_item_control),
                                  reinterpret_cast<void*>(&reset_stuff_execute),
                                  reinterpret_cast<void**>(&g_backup_reset_stuff),
                                  "UIMix_ResetStuffItemControl")) {
            QOL_LOG_WARN(kDomain, "reset-stuff hook skipped reason=install_failed");
        }
    }

    g_installed.store(true, std::memory_order_release);
    QOL_LOG_INFO(kDomain,
                 "craft ui hooks installed core=5 desc=%d slotcount=%d label=%d title=%d tab=%d "
                 "reset=%d",
                 g_backup_set_desc_text != nullptr ? 1 : 0,
                 g_backup_item_draw_porting != nullptr ? 1 : 0,
                 g_backup_button_recipe_draw != nullptr ? 1 : 0,
                 g_backup_uimix_draw != nullptr ? 1 : 0,
                 g_backup_menu_list_draw != nullptr ? 1 : 0,
                 g_backup_reset_stuff != nullptr ? 1 : 0);
    return true;
}
