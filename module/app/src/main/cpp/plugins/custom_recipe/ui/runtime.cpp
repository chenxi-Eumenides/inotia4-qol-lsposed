#include "runtime.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../feature.h"
#include "../result.h"
#include "../text_table.h"
#include "feature.h"

// ===========================================================================
// 合成器界面子层运行时：全部 UIMix hook 的唯一挂载处（原 craft_ui 并入 custom_recipe）
// ===========================================================================
//
// 分层（界面归界面，结果归结果）：
//   本文件 = **界面**。配方判定与扣料/产物决策调用同插件父命名空间的结果 API
//   （custom_recipe::craft_result_api()），不再经 capability registry。
//
// 挂钩清单（同一 UIMix 函数只挂一次，声明的 12 条资源由宿主安装）：
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

namespace qol::features::custom_recipe::ui {
namespace {

constexpr const char* kOwner = "custom_recipe";

// ---- hook 资源 id（与声明一一对应；on_resource_ready 只接受这些）----
constexpr const char* kResourcePlace = "hook:UIMix_ButtonInvenItemSelectExe";
constexpr const char* kResourceMixing = "hook:UIMix_ButtonMixingExe";
constexpr const char* kResourceMakeItem = "hook:MIXSYSTEM_MakeItem";
constexpr const char* kResourceMenu = "hook:UIMix_ButtonMenuListExe";
constexpr const char* kResourceRecipe = "hook:UIMix_ButtonRecipeExe";
constexpr const char* kResourceSetDescText = "hook:X_TEXTCTRL_SetTextControl";
constexpr const char* kResourceControlItemDraw = "hook:ControlItem_Draw";
constexpr const char* kResourceItemDrawPorting = "hook:ITEM_DrawPorting";
constexpr const char* kResourceButtonRecipeDraw = "hook:UIMix_ButtonRecipeDraw";
constexpr const char* kResourceUimixDraw = "hook:UIMix_Draw";
constexpr const char* kResourceMenuListDraw = "hook:UIMix_ButtonMenuListDraw";
constexpr const char* kResourceResetStuff = "hook:UIMix_ResetStuffItemControl";

// ---- hook 跳板（由宿主经 on_resource_ready 交还写入）----
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

// 3 格填入格「不显示整堆数量」：ControlItem_Draw 识别到本模式填入格时置位，ITEM_DrawPorting
// 把 show_count 压成 0（只影响本模式填入格；背包等其它界面数量照常）。
std::atomic<bool> g_drawing_three_slot_slot{false};

// 结果 API 在父命名空间；同进程直接调用，不做能力查找。
const result::CraftResultApi* craft_api() { return &craft_result_api(); }

bool core_backups_ready_impl() {
    return g_backup_place != nullptr && g_backup_mixing != nullptr &&
           g_backup_make_item != nullptr && g_backup_menu != nullptr &&
           g_backup_recipe != nullptr;
}

// 物品类别 = `item + I_TYPE`(u16) 的 bits6-15；对用到的类别而言 == itemId。
bool new_game_item_handle(void* item, contracts::QolItemHandle* out) {
    const auto* sdk = host_sdk();
    if (item == nullptr || out == nullptr || sdk == nullptr) return false;
    uint32_t owner = 0;
    if (sdk->item_handle.lookup_item != nullptr &&
        sdk->item_handle.lookup_item(item, out, &owner) == contracts::kQolItemHandleStatusOk) {
        return owner == contracts::kQolItemHandleOwnerGame && out->slot != 0 &&
               out->generation != 0;
    }
    return sdk->item_handle.register_item != nullptr &&
           sdk->item_handle.register_item(item, contracts::kQolItemHandleOwnerGame, out) ==
               contracts::kQolItemHandleStatusOk && out->slot != 0 && out->generation != 0;
}

uint16_t item_category(contracts::QolItemHandle handle) {
    int32_t category = 0;
    const auto* facts = item_facts();
    if (handle.slot == 0 || handle.generation == 0 || facts == nullptr ||
        facts->category == nullptr ||
        facts->category(handle, &category).ok != kQolOk || category < 0) {
        return 0;
    }
    return static_cast<uint16_t>(category);
}

// 宝石档位（28..32 → 1..5；其它返回 0）。
int jewel_grade(uint16_t category) {
    if (category < 28 || category > 32) return 0;
    return static_cast<int>(category) - 27;
}

bool item_is_stackable(contracts::QolItemHandle handle) {
    int32_t is_real_equip = 0;
    const auto* facts = item_facts();
    if (handle.slot == 0 || handle.generation == 0 || facts == nullptr ||
        facts->is_real_equip == nullptr ||
        facts->is_real_equip(handle, &is_real_equip).ok != kQolOk) {
        return false;
    }
    return is_real_equip == 0;
}

// 失败提示：本插件自有文案经本地 scope 生效，不改动共享文本表内容。
void craft_show_quality_unavailable() {
    TextScopeGuard scope(kTextScopeQualityUnavailable);
    craft_show_text(kQualityUnavailablePopupId);
}

void craft_show_material_consumed() {
    TextScopeGuard scope(kTextScopeMaterialMayHaveBeenConsumed);
    craft_show_text(kMaterialMayHaveBeenConsumedPopupId);
}

void craft_show_product_prepare_failed() {
    TextScopeGuard scope(kTextScopeProductPrepareFailed);
    craft_show_text(kProductPrepareFailedPopupId);
}

// 3 格合成的 YesNo 确认回调（经 x3 传入，void() 形态；原版该槽位是 UIMix_StartMix@0xc0870）。
// 上下文只能用模块自有全局（x6/param 会被原版当钱数渲染）。结果由结果层回传，界面按枚举收尾。
void on_confirm_three_slot() {
    const result::CraftResultApi* api = craft_api();
    result::CraftExecuteInput in{};
    craft_read_stuff_categories(in.slot_categories);
    for (size_t i = 0; i < kStuffSlotCount; ++i) {
        in.items[i] = craft_stuff_item(i);
        if (in.items[i].slot != 0 && in.items[i].generation != 0) {
            in.held_counts[i] = craft_category_held_count(item_category(in.items[i]));
        }
    }
    switch (api->execute(&in)) {
        case result::CraftExecuteResult::kOk:
            finish_three_slot_craft();
            return;
        case result::CraftExecuteResult::kStaleSlots:
        case result::CraftExecuteResult::kAborted:
            // 确认框停留期间格被换掉 / 停用 / 无待确认：不消耗、不产出 → 提示 + 清空重来。
            craft_show_text(kTextInsufficientMaterial);
            reset_stuff_and_refresh();
            return;
        case result::CraftExecuteResult::kNotEnoughHeld:
            // 扣料前库存复核失败：不消耗、不产出（保留已填格，便于玩家补材料后重试）。
            craft_show_text(kTextInsufficientMaterial);
            return;
        case result::CraftExecuteResult::kPrecheckFailed:
            // 扣料前的前置校验失败：不消耗、不产出（保留已填格）。
            craft_show_text(kTextInsufficientMaterial);
            return;
        case result::CraftExecuteResult::kMaterialRemoveFailed:
        case result::CraftExecuteResult::kMaterialObservationFailed:
        case result::CraftExecuteResult::kProductSaveFailed:
            // 扣料后阶段：材料可能已扣，且 save 后产物所有权未知；不得提示空间不足，
            // 也不得断言「无产物入包」，只报告材料可能已扣。
            craft_show_material_consumed();
            reset_stuff_and_refresh();
            return;
        case result::CraftExecuteResult::kProductPrepareFailed:
            // 产物准备在扣料前失败：材料未扣，不宣称空间不足或无产物入包。
            craft_show_product_prepare_failed();
            reset_stuff_and_refresh();
            return;
        case result::CraftExecuteResult::kNoBagSpace:
        default:
            // 遗留/未知结果：只提示空间不足 + 清空重来。
            craft_show_text(kTextNoBagSpace);
            reset_stuff_and_refresh();
            return;
    }
}

// ---------------------------------------------------------------------------
// wrapper：放料（UIMix_ButtonInvenItemSelectExe）
// ---------------------------------------------------------------------------
void place_execute(void* ctrl) {
    if (g_backup_place == nullptr) return;
    const result::CraftResultApi* api = craft_api();
    const uint32_t mix_type = craft_mix_type();
    // 3 格隐式配方（type 1「合成」入口）：任意物品可填入 ⇒ 原版三道校验不可用，整段自实现。
    if (api->three_slot_mode_active(mix_type)) {
        const int64_t index = craft_selected_stuff_index();
        const contracts::QolItemHandle item = craft_selected_inventory_item();
        result::CraftPlaceInput in{};
        in.slot_index = index;
        in.item = item;
        if (item.slot != 0 && item.generation != 0) {
            in.item_category = item_category(item);
            in.item_stackable = item_is_stackable(item);
            for (size_t i = 0; i < kStuffSlotCount; ++i) {
                if (static_cast<int64_t>(i) == index) continue;
                const contracts::QolItemHandle other = craft_stuff_item(i);
                const bool other_valid = other.slot != 0 && other.generation != 0;
                if (other_valid && other.slot == item.slot &&
                    other.generation == item.generation) ++in.placed_same_object;
                if (other_valid && item_category(other) == in.item_category) {
                    ++in.same_category_placed;
                }
            }
            in.category_held_count = craft_category_held_count(in.item_category);
        }
        switch (craft_place_disposition(api->place(&in))) {
            case CraftPlaceDisposition::kAlreadyPlaced:
                craft_show_text(kTextAlreadyPlaced);  // 99：不可堆叠物品不能重复放置
                return;
            case CraftPlaceDisposition::kNotEnoughHeld:
                // 94：持有总数不足，无法再占一格。放料失败**不清空**已填格。
                craft_show_text(kTextInsufficientMaterial);
                return;
            case CraftPlaceDisposition::kWriteFailed:
                log_message(kLogWarn, "three slot place blocked reason=stuff write failed");
                return;
            case CraftPlaceDisposition::kAdvance:
                craft_select_first_empty_stuff_slot();  // 放料后自动选中下一空格
                return;
            case CraftPlaceDisposition::kStop:
                return;  // 不可能：上面已判定处于 3 格模式
        }
        return;
    }
    // def 非空即代表「当前选中的是模块注入的 kJewelTierUp 配方」，与总开关无关
    //（停用后旧面板仍可能持有它，必须能识别以 fail-closed）。
    if (!api->is_jewel_tier_up_recipe(mix_type)) {
        g_backup_place(ctrl);
        return;
    }
    if (!api->enabled()) {
        // 停用后残留的旧面板仍可能指向注入配方：原版会照常写入目标槽并算费用，
        // 且合成时会走原版通用路径白扣材料 —— 故转调后立即撤销放置。
        g_backup_place(ctrl);
        craft_set_target_slot_item(contracts::kInvalidItemHandle);
        return;
    }
    g_backup_place(ctrl);  // 原版：CheckMixture（注入 mixType 恒返回 0）→ SetItem → 写费用

    const contracts::QolItemHandle item = craft_target_slot_item();
    if (item.slot == 0 || item.generation == 0) return;  // 本次未放入任何物品
    if (!api->is_jewel_item(item)) {
        craft_set_target_slot_item(contracts::kInvalidItemHandle);  // 撤销放置
        craft_show_text(kTextOnlyJewel);
        return;
    }
    // 宝石：按宝石档位 + 角色等级写入材料需求数（下一帧绘制即显示），再强制费用 0。
    const uint16_t category = item_category(item);
    result::CraftMaterialNeedInput ni{};
    ni.mix_type = mix_type;
    ni.jewel_grade = jewel_grade(category);
    ni.character_level = craft_character_level();
    result::CraftMaterialNeedOutput no{};
    api->material_needs(&ni, &no);
    for (int i = 0; i < no.count; ++i) {
        craft_write_stuff_need(static_cast<size_t>(i),
                               item_category(craft_stuff_item(static_cast<size_t>(i))),
                               static_cast<uint16_t>(no.needs[i]));
    }
    craft_set_cost_zero();
    if (log_budget_take()) {
        log_message(kLogDebug, "place accepted jewel mixType=%u cost=0", mix_type);
    }
}

// ---------------------------------------------------------------------------
// wrapper：合成按钮（UIMix_ButtonMixingExe）
// ---------------------------------------------------------------------------
void craft_execute(void* ctrl) {
    if (g_backup_mixing == nullptr) return;
    const result::CraftResultApi* api = craft_api();
    const uint32_t mix_type = craft_mix_type();
    // 3 格隐式配方：合成前先按 3 格内容查表。命中 → 弹原版 YesNo、回调走模块产出流程；
    // 未命中 → 弹 94 + 清空重来。两条路径都**不**转调原函数。
    if (api->three_slot_mode_active(mix_type)) {
        result::CraftPrepareInput in{};
        craft_read_stuff_categories(in.slot_categories);
        switch (api->prepare(&in)) {
            case result::PrepareOutcome::kNoMatch:
                if (log_budget_take()) {
                    log_message(kLogDebug, "three slot miss slots=%u,%u,%u", in.slot_categories[0],
                                  in.slot_categories[1], in.slot_categories[2]);
                }
                craft_show_text(kTextInsufficientMaterial);
                reset_stuff_and_refresh();
                return;
            case result::PrepareOutcome::kConfirmPending:
                if (!craft_show_yesno(kTextConfirmCraft, &on_confirm_three_slot, nullptr)) {
                    log_message(kLogWarn, "three slot confirm skipped reason=no_popup");
                }
                return;
            case result::PrepareOutcome::kNotMine:
                return;  // 不可能：上面已判定处于 3 格模式
        }
        return;
    }
    if (!api->is_jewel_tier_up_recipe(mix_type)) {
        g_backup_mixing(ctrl);
        return;
    }
    if (!api->enabled()) {
        // fail-closed：不允许经残留面板对该 mixType 发起合成（原版通用路径产物无意义、照扣材料）。
        log_message(kLogWarn, "craft blocked reason=disabled mixType=%u", mix_type);
        return;
    }
    switch (api->craft_gate(craft_target_slot_item())) {
        case result::CraftGate::kNotJewel:
            craft_show_text(kTextOnlyJewel);
            return;
        case result::CraftGate::kAlreadyGold:
            craft_show_text(kTextMaxCrafted);
            return;
        case result::CraftGate::kUnavailable:
            // 质量服务不可用：与「真正非宝石」区分提示，只阻断宝石升阶。
            craft_show_quality_unavailable();
            return;
        case result::CraftGate::kPass:
        default:
            break;
    }
    g_backup_mixing(ctrl);  // 费用 0 时原版自动跳过金币校验并弹确认框 → StartMix
}

// ---------------------------------------------------------------------------
// wrapper：产物（MIXSYSTEM_MakeItem）
// ---------------------------------------------------------------------------
QolResult craft_rand_inclusive(int32_t lo, int32_t hi, int32_t* out) {
    return random_inclusive(lo, hi, out);  // 父命名空间的游戏闭区间随机端口
}

int make_item_execute(int32_t mix_type, void** out_item) {
    int32_t result = -1;
    const result::CraftResultApi* api = craft_api();
    if (out_item != nullptr && *out_item != nullptr) {
        result::CraftMakeItemInput in{};
        in.mix_type = static_cast<uint32_t>(mix_type);
        if (!new_game_item_handle(*out_item, &in.item)) return 1;
        in.rand_inclusive = &craft_rand_inclusive;
        result = api->make_item(&in);
    }
    if (result >= 0) return result;
    return (g_backup_make_item != nullptr) ? g_backup_make_item(mix_type, out_item) : 1;
}

// ---------------------------------------------------------------------------
// wrapper：页签（UIMix_ButtonMenuListExe）
// ---------------------------------------------------------------------------
void menu_execute(void* ctrl) {
    if (g_backup_menu != nullptr) g_backup_menu(ctrl);
    select_on_view_enter();
}

// ---------------------------------------------------------------------------
// wrapper：配方点击（UIMix_ButtonRecipeExe）
// ---------------------------------------------------------------------------
void recipe_execute(void* ctrl) {
    if (g_backup_recipe == nullptr) return;
    const int64_t clicked_type = craft_ui_type();
    const int64_t prev_selected = craft_selected_recipe_at(clicked_type);
    g_backup_recipe(ctrl);

    const result::CraftResultApi* api = craft_api();
    const uint32_t mix_type = craft_mix_type();
    const int64_t form_type = api->form_ui_type_for_mix_type(mix_type);
    if (form_type >= 0 && form_type != craft_ui_type()) {
        craft_set_ui_type(form_type);
        craft_init_mixing_state();
        craft_reset_stuff_item_control();
        craft_set_selected_recipe_at(clicked_type, prev_selected);
        if (log_budget_take()) {
            log_message(kLogDebug, "custom recipe picked mixType=%u type=%lld", mix_type,
                          static_cast<long long>(form_type));
        }
    }
    select_on_view_enter();
}

// ---------------------------------------------------------------------------
// wrapper：描述文案替换（X_TEXTCTRL_SetTextControl）
// ---------------------------------------------------------------------------
// 本 hook 可能处于 UI 事件路径，不得有任何级别日志（R4；诊断走 debug + 有界预算）。
void set_desc_text_execute(void* ctrl, const char* text, uint32_t width, uint32_t f1, uint32_t f2,
                           int32_t f3) {
    if (g_backup_set_desc_text == nullptr) return;
    const result::CraftResultApi* api = craft_api();
    if (api->desc_should_replace(craft_mix_type(), text)) {
        constexpr size_t kLen = sizeof(kModuleRecipeDesc) - 1;
        auto* buf = const_cast<char*>(text);
        for (size_t n = 0; n < kLen; ++n) buf[n] = kModuleRecipeDesc[n];
        buf[kLen] = '\0';
        if (log_budget_take()) {
            log_message(kLogDebug, "desc replaced mixType=%u", craft_mix_type());
        }
    }
    g_backup_set_desc_text(ctrl, text, width, f1, f2, f3);
}

void control_item_draw_execute(void* ctrl) {
    if (g_backup_control_item_draw == nullptr) return;
    bool is_three_slot = false;
    const result::CraftResultApi* api = craft_api();
    if (api->three_slot_mode_active(craft_mix_type()) && is_stuff_ctrl(ctrl)) {
        is_three_slot = true;
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
void button_recipe_draw_execute(void* ctrl) {
    TextScopeGuard scope(kTextScopeRecipeButton);
    if (g_backup_button_recipe_draw != nullptr) g_backup_button_recipe_draw(ctrl);
}

void uimix_draw_execute() {
    TextScopeGuard scope(kTextScopePanelTitle);
    if (g_backup_uimix_draw != nullptr) g_backup_uimix_draw();
}

void menu_list_draw_execute(void* ctrl) {
    TextScopeGuard scope(kTextScopePageTab);
    if (g_backup_menu_list_draw != nullptr) g_backup_menu_list_draw(ctrl);
}

// ---------------------------------------------------------------------------
// wrapper：清空填入格（UIMix_ResetStuffItemControl）——**不随开关门控**
// ---------------------------------------------------------------------------
void reset_stuff_execute() {
    if (g_backup_reset_stuff == nullptr) return;
    g_backup_reset_stuff();
    craft_select_first_empty_stuff_slot();
}

// ---------------------------------------------------------------------------
// 12 条资源声明（5 核心 required/fail-closed + 7 装饰 optional/fail-soft）
// ---------------------------------------------------------------------------
// gate=nullptr：资源跟随插件 lifecycle（唯一开关 custom_recipe.recipes）。
#define CRAFT_CLAIM(ID, SYM, HANDLER, REQ, FAIL)                                         \
    {                                                                                     \
        ID, SYM, contracts::ResourceMechanism::kEntryHook, contracts::ResourcePolicy::kPrivate, \
            REQ, FAIL, true, kOwner,                                                      \
            "UIMix 合成器界面 hook；wrapper 只做界面判断/呈现，配方判定经内部结果 API", \
            nullptr, 120, reinterpret_cast<void*>(HANDLER)                                \
    }

const contracts::ResourceClaim kClaims[] = {
    // 5 核心：任一失败即整体回滚（required/fail-closed）。
    CRAFT_CLAIM("hook:UIMix_ButtonInvenItemSelectExe", "F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA",
                &place_execute, contracts::ResourceRequirement::kRequired,
                contracts::ResourceFailure::kFailClosed),
    CRAFT_CLAIM("hook:UIMix_ButtonMixingExe", "F_UIMIX_BUTTON_MIXING_EXE_VMA", &craft_execute,
                contracts::ResourceRequirement::kRequired,
                contracts::ResourceFailure::kFailClosed),
    CRAFT_CLAIM("hook:MIXSYSTEM_MakeItem", "F_MAKE_MIX_VMA", &make_item_execute,
                contracts::ResourceRequirement::kRequired,
                contracts::ResourceFailure::kFailClosed),
    CRAFT_CLAIM("hook:UIMix_ButtonMenuListExe", "F_UIMIX_BUTTON_MENU_LIST_EXE_VMA", &menu_execute,
                contracts::ResourceRequirement::kRequired,
                contracts::ResourceFailure::kFailClosed),
    CRAFT_CLAIM("hook:UIMix_ButtonRecipeExe", "F_UIMIX_BUTTON_RECIPE_EXE_VMA", &recipe_execute,
                contracts::ResourceRequirement::kRequired,
                contracts::ResourceFailure::kFailClosed),
    // 7 装饰：失败只 degraded、不拖垮核心链。
    CRAFT_CLAIM("hook:X_TEXTCTRL_SetTextControl", "F_XTEXTCTRL_SET_TEXT_CONTROL_VMA",
                &set_desc_text_execute, contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
    CRAFT_CLAIM("hook:ControlItem_Draw", "F_CONTROL_ITEM_DRAW_VMA", &control_item_draw_execute,
                contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
    CRAFT_CLAIM("hook:ITEM_DrawPorting", "F_ITEM_DRAW_PORTING_VMA", &item_draw_porting_execute,
                contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
    CRAFT_CLAIM("hook:UIMix_ButtonRecipeDraw", "F_UIMIX_BUTTON_RECIPE_DRAW_VMA",
                &button_recipe_draw_execute, contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
    CRAFT_CLAIM("hook:UIMix_Draw", "F_UIMIX_DRAW_VMA", &uimix_draw_execute,
                contracts::ResourceRequirement::kOptional, contracts::ResourceFailure::kFailSoft),
    CRAFT_CLAIM("hook:UIMix_ButtonMenuListDraw", "F_UIMIX_BUTTON_MENU_LIST_DRAW_VMA",
                &menu_list_draw_execute, contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
    CRAFT_CLAIM("hook:UIMix_ResetStuffItemControl", "F_UIMIX_RESET_STUFF_ITEM_CONTROL_VMA",
                &reset_stuff_execute, contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
};
#undef CRAFT_CLAIM

constexpr uint32_t kClaimCount = sizeof(kClaims) / sizeof(kClaims[0]);

void clear_backups() {
    g_backup_place = nullptr;
    g_backup_mixing = nullptr;
    g_backup_make_item = nullptr;
    g_backup_menu = nullptr;
    g_backup_recipe = nullptr;
    g_backup_set_desc_text = nullptr;
    g_backup_control_item_draw = nullptr;
    g_backup_item_draw_porting = nullptr;
    g_backup_button_recipe_draw = nullptr;
    g_backup_uimix_draw = nullptr;
    g_backup_menu_list_draw = nullptr;
    g_backup_reset_stuff = nullptr;
}

int32_t on_resource_ready_impl(const char* resource_id, void* backup) {
    if (resource_id == nullptr) return 1;
    if (std::strcmp(resource_id, kResourcePlace) == 0) {
        g_backup_place = reinterpret_cast<UIMixButtonInvenItemSelectExeFn>(backup);
    } else if (std::strcmp(resource_id, kResourceMixing) == 0) {
        g_backup_mixing = reinterpret_cast<UIMixButtonMixingExeFn>(backup);
    } else if (std::strcmp(resource_id, kResourceMakeItem) == 0) {
        g_backup_make_item = reinterpret_cast<MakeMixFn>(backup);
    } else if (std::strcmp(resource_id, kResourceMenu) == 0) {
        g_backup_menu = reinterpret_cast<UIMixButtonMenuListExeFn>(backup);
    } else if (std::strcmp(resource_id, kResourceRecipe) == 0) {
        g_backup_recipe = reinterpret_cast<UIMixButtonRecipeExeFn>(backup);
    } else if (std::strcmp(resource_id, kResourceSetDescText) == 0) {
        g_backup_set_desc_text = reinterpret_cast<XTextCtrlSetTextControlFn>(backup);
    } else if (std::strcmp(resource_id, kResourceControlItemDraw) == 0) {
        g_backup_control_item_draw = reinterpret_cast<ControlItemDrawFn>(backup);
    } else if (std::strcmp(resource_id, kResourceItemDrawPorting) == 0) {
        g_backup_item_draw_porting = reinterpret_cast<ItemDrawPortingFn>(backup);
    } else if (std::strcmp(resource_id, kResourceButtonRecipeDraw) == 0) {
        g_backup_button_recipe_draw = reinterpret_cast<UIMixButtonRecipeDrawFn>(backup);
    } else if (std::strcmp(resource_id, kResourceUimixDraw) == 0) {
        g_backup_uimix_draw = reinterpret_cast<UIMixDrawFn>(backup);
    } else if (std::strcmp(resource_id, kResourceMenuListDraw) == 0) {
        g_backup_menu_list_draw = reinterpret_cast<UIMixButtonMenuListDrawFn>(backup);
    } else if (std::strcmp(resource_id, kResourceResetStuff) == 0) {
        g_backup_reset_stuff = reinterpret_cast<UIMixResetStuffItemControlFn>(backup);
    } else {
        return 1;  // 未知资源：fail-closed（宿主按 resource_ready_failed 回滚）
    }
    return 0;
}

}  // namespace

const contracts::ResourceClaim* claims(uint32_t* out_count) {
    if (out_count != nullptr) *out_count = kClaimCount;
    return kClaims;
}

bool core_backups_ready() { return core_backups_ready_impl(); }

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    if (ctx.sdk == nullptr) return 1;
    set_context(ctx);
    return on_resource_ready_impl(resource_id, backup);
}

bool bind(contracts::PluginContext& ctx, const contracts::ItemFactsApi* item_facts,
          const contracts::CraftFactsApi* craft_facts,
          const contracts::QolCustomRecipeWritePort* recipe_write_port,
          const contracts::QolCraftUiWritePort* ui_write_port,
          const contracts::TextLayerApi* text_api) {
    (void)recipe_write_port;
    (void)text_api;
    if (ctx.sdk == nullptr || item_facts == nullptr || craft_facts == nullptr ||
        ui_write_port == nullptr) {
        clear();
        return false;
    }
    set_item_facts(item_facts);
    set_craft_facts(craft_facts);
    set_write_port(ui_write_port);
    set_context(ctx);
    return true;
}

void clear() {
    set_item_facts(nullptr);
    set_craft_facts(nullptr);
    set_write_port(nullptr);
    g_drawing_three_slot_slot.store(false, std::memory_order_release);
    clear_backups();
}

}  // namespace qol::features::custom_recipe::ui
