#include "plugin.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../../contracts/craft_result.h"
#include "../../contracts/craft_facts.h"
#include "../../contracts/game_ports/craft_ui_write_port.h"
#include "feature.h"

// ===========================================================================
// 合成器界面层插件：全部 UIMix hook 的唯一挂载处（C2 Step 3）
// ===========================================================================
//
// 分层（界面归界面，结果归结果）：
//   本文件 = **界面**。配方判定与扣料/产物决策一律经能力契约 `craft_result` 问 provider
//   （plugins/custom_recipe），并按回传枚举决定弹什么文案、要不要清空填入格。
//   反向依赖已消除：本层不 include 任何 custom_recipe 头。
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

namespace qol::features::craft_ui {
namespace {

constexpr const char* kPluginId = "craft_ui";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kOwner = "craft_ui";
constexpr const char* kGateKey = "customRecipeEnabled";

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

// 消费方式：每次调用现取能力、不缓存跨帧指针（provider 撤销期 require 返回 nullptr → 降级）。
const contracts::CraftResultApi* craft_api() {
    const contracts::QolHostSdk* sdk = host_sdk();
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = sdk->capabilities.require(
        contracts::kCraftResultCapabilityId,
        {static_cast<uint16_t>(contracts::kCraftResultVersionMajor),
         static_cast<uint16_t>(contracts::kCraftResultVersionMinor)},
        &iface);
    return result.ok == kQolOk ? static_cast<const contracts::CraftResultApi*>(iface) : nullptr;
}

// 物品类别 = `item + I_TYPE`(u16) 的 bits6-15；对用到的类别而言 == itemId。
bool item_handle(void* item, contracts::QolItemHandle* out) {
    const auto* sdk = host_sdk();
    if (item == nullptr || out == nullptr || sdk == nullptr) return false;
    uint32_t owner = 0;
    if (sdk->item_handle.lookup_item != nullptr &&
        sdk->item_handle.lookup_item(item, out, &owner) == contracts::kQolItemHandleStatusOk) {
        return true;
    }
    return sdk->item_handle.register_item != nullptr &&
           sdk->item_handle.register_item(item, contracts::kQolItemHandleOwnerGame, out) ==
               contracts::kQolItemHandleStatusOk && out->slot != 0 && out->generation != 0;
}

uint16_t item_category(void* item) {
    int32_t category = 0;
    contracts::QolItemHandle handle{};
    const auto* facts = item_facts();
    if (!item_handle(item, &handle) || facts == nullptr || facts->category == nullptr ||
        facts->category(handle, &category).ok != kQolOk || category < 0) {
        return 0;
    }
    return static_cast<uint16_t>(category);
}

// 宝石档位（28..32 → 1..5；其它返回 0）——与 plugins/custom_recipe/rules.h 的纯规则同式，
// 界面层只需此单一映射即可构造 material_needs 入参。
int jewel_grade(uint16_t category) {
    if (category < 28 || category > 32) return 0;
    return static_cast<int>(category) - 27;
}

bool item_is_stackable(void* item) {
    int32_t is_real_equip = 0;
    contracts::QolItemHandle handle{};
    const auto* facts = item_facts();
    if (!item_handle(item, &handle) || facts == nullptr || facts->is_real_equip == nullptr ||
        facts->is_real_equip(handle, &is_real_equip).ok != kQolOk) {
        return false;
    }
    return is_real_equip == 0;
}

// 3 格合成的 YesNo 确认回调（经 x3 传入，void() 形态；原版该槽位是 UIMix_StartMix@0xc0870）。
// 上下文只能用模块自有全局（x6/param 会被原版当钱数渲染）。结果由能力层回传，界面按枚举收尾。
void on_confirm_three_slot() {
    const contracts::CraftResultApi* api = craft_api();
    if (api == nullptr) {
        // provider 撤销期间：按「材料不足」保守收尾（不消耗、不产出）。
        craft_show_text(kTextInsufficientMaterial);
        reset_stuff_and_refresh();
        return;
    }
    contracts::CraftExecuteInput in{};
    craft_read_stuff_categories(in.slot_categories);
    for (size_t i = 0; i < kStuffSlotCount; ++i) {
        in.items[i] = craft_stuff_item(i);
        if (in.items[i] != nullptr) {
            in.held_counts[i] =
                craft_category_held_count(item_category(in.items[i]));
        }
    }
    switch (api->execute(&in)) {
        case contracts::CraftExecuteResult::kOk:
            finish_three_slot_craft();
            return;
        case contracts::CraftExecuteResult::kStaleSlots:
        case contracts::CraftExecuteResult::kAborted:
            // 确认框停留期间格被换掉 / 停用 / 无待确认：不消耗、不产出 → 提示 + 清空重来。
            craft_show_text(kTextInsufficientMaterial);
            reset_stuff_and_refresh();
            return;
        case contracts::CraftExecuteResult::kNotEnoughHeld:
            // 扣料前库存复核失败：不消耗、不产出（保留已填格，便于玩家补材料后重试）。
            craft_show_text(kTextInsufficientMaterial);
            return;
        case contracts::CraftExecuteResult::kNoBagSpace:
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
    const contracts::CraftResultApi* api = craft_api();
    if (api == nullptr) {
        if (craft_result_unavailable_is_fail_closed(config_enabled(kGateKey))) {
            log_message(kLogWarn, "place blocked reason=craft_result unavailable");
            return;
        }
        g_backup_place(ctrl);
        return;
    }
    const uint32_t mix_type = craft_mix_type();
    // 3 格隐式配方（type 1「合成」入口）：任意物品可填入 ⇒ 原版三道校验不可用，整段自实现。
    if (api->three_slot_mode_active(mix_type)) {
        refresh_stuff_ctrls();  // 面板必然打开：刷新填入格控件缓存（绘制层只比较、不解引用）
        const int64_t index = craft_selected_stuff_index();
        void* item = craft_selected_inventory_item();
        contracts::CraftPlaceInput in{};
        in.slot_index = index;
        in.item = item;
        if (item != nullptr) {
            in.item_category = item_category(item);
            in.item_stackable = item_is_stackable(item);
            for (size_t i = 0; i < kStuffSlotCount; ++i) {
                if (static_cast<int64_t>(i) == index) continue;
                void* other = craft_stuff_item(i);
                if (other == item) ++in.placed_same_object;
                if (other != nullptr && item_category(other) == in.item_category) {
                    ++in.same_category_placed;
                }
            }
            in.category_held_count = craft_category_held_count(in.item_category);
        }
        switch (api->place(&in)) {
            case contracts::PlaceOutcome::kAlreadyPlaced:
                craft_show_text(kTextAlreadyPlaced);  // 99：不可堆叠物品不能重复放置
                return;
            case contracts::PlaceOutcome::kNotEnoughHeld:
                // 94：持有总数不足，无法再占一格。放料失败**不清空**已填格。
                craft_show_text(kTextInsufficientMaterial);
                return;
            case contracts::PlaceOutcome::kPlaced:
                craft_select_first_empty_stuff_slot();  // 放料后自动选中下一空格
                return;
            case contracts::PlaceOutcome::kNotMine:
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
        craft_set_target_slot_item(nullptr);
        return;
    }
    g_backup_place(ctrl);  // 原版：CheckMixture（注入 mixType 恒返回 0）→ SetItem → 写费用

    void* item = craft_target_slot_item();
    if (item == nullptr) return;  // 本次未放入任何物品
    if (!api->is_jewel_item(item)) {
        craft_set_target_slot_item(nullptr);  // 撤销放置
        craft_show_text(kTextOnlyJewel);
        return;
    }
    // 宝石：按宝石档位 + 角色等级写入材料需求数（下一帧绘制即显示），再强制费用 0。
    const uint16_t category = item_category(item);
    contracts::CraftMaterialNeedInput ni{};
    ni.mix_type = mix_type;
    ni.jewel_grade = jewel_grade(category);
    ni.character_level = craft_character_level();
    contracts::CraftMaterialNeedOutput no{};
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
    const contracts::CraftResultApi* api = craft_api();
    if (api == nullptr) {
        if (craft_result_unavailable_is_fail_closed(config_enabled(kGateKey))) {
            log_message(kLogWarn, "craft blocked reason=craft_result unavailable");
            return;
        }
        g_backup_mixing(ctrl);
        return;
    }
    const uint32_t mix_type = craft_mix_type();
    // 3 格隐式配方：合成前先按 3 格内容查表。命中 → 弹原版 YesNo、回调走模块产出流程；
    // 未命中 → 弹 94 + 清空重来。两条路径都**不**转调原函数。
    if (api->three_slot_mode_active(mix_type)) {
        refresh_stuff_ctrls();
        contracts::CraftPrepareInput in{};
        craft_read_stuff_categories(in.slot_categories);
        switch (api->prepare(&in)) {
            case contracts::PrepareOutcome::kNoMatch:
                if (log_budget_take()) {
                    log_message(kLogDebug, "three slot miss slots=%u,%u,%u", in.slot_categories[0],
                                  in.slot_categories[1], in.slot_categories[2]);
                }
                craft_show_text(kTextInsufficientMaterial);
                reset_stuff_and_refresh();
                return;
            case contracts::PrepareOutcome::kConfirmPending:
                if (!craft_show_yesno(
                        kTextConfirmCraft, reinterpret_cast<void*>(&on_confirm_three_slot),
                        nullptr)) {
                    log_message(kLogWarn, "three slot confirm skipped reason=no_popup");
                }
                return;
            case contracts::PrepareOutcome::kNotMine:
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
        case contracts::CraftGate::kNotJewel:
            craft_show_text(kTextOnlyJewel);
            return;
        case contracts::CraftGate::kAlreadyGold:
            craft_show_text(kTextMaxCrafted);
            return;
        case contracts::CraftGate::kPass:
            break;
    }
    g_backup_mixing(ctrl);  // 费用 0 时原版自动跳过金币校验并弹确认框 → StartMix
}

// ---------------------------------------------------------------------------
// wrapper：产物（MIXSYSTEM_MakeItem）
// ---------------------------------------------------------------------------
int64_t craft_rand64_adapter(int lo, int hi) {
    return lo <= hi ? lo : hi;
}

int make_item_execute(int32_t mix_type, void** out_item) {
    int32_t result = -1;
    const contracts::CraftResultApi* api = craft_api();
    if (api == nullptr &&
        craft_result_unavailable_is_fail_closed(config_enabled(kGateKey))) {
        log_message(kLogWarn, "make item blocked reason=craft_result unavailable");
        return 1;
    }
    if (api != nullptr && out_item != nullptr) {
        contracts::CraftMakeItemInput in{};
        in.mix_type = static_cast<uint32_t>(mix_type);
        in.item = *out_item;
        in.rand_inclusive = &craft_rand64_adapter;
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

    const contracts::CraftResultApi* api = craft_api();
    if (api != nullptr) {
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
    const contracts::CraftResultApi* api = craft_api();
    if (api != nullptr && api->desc_should_replace(craft_mix_type(), text)) {
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
    const contracts::CraftResultApi* api = craft_api();
    if (api != nullptr && api->three_slot_mode_active(craft_mix_type()) &&
        is_stuff_ctrl(ctrl)) {
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
    TextScopeGuard scope(2);
    if (g_backup_button_recipe_draw != nullptr) g_backup_button_recipe_draw(ctrl);
}

void uimix_draw_execute() {
    TextScopeGuard scope(3);
    if (g_backup_uimix_draw != nullptr) g_backup_uimix_draw();
}

void menu_list_draw_execute(void* ctrl) {
    TextScopeGuard scope(4);
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
#define CRAFT_CLAIM(ID, SYM, HANDLER, GATE, REQ, FAIL)                                   \
    {                                                                                     \
        ID, SYM, contracts::ResourceMechanism::kEntryHook, contracts::ResourcePolicy::kPrivate, \
            REQ, FAIL, true, kOwner,                                                      \
            "UIMix 合成器界面 hook；wrapper 只做界面判断/呈现，配方判定经 craft_result 能力", \
            GATE, 120, reinterpret_cast<void*>(HANDLER)                                   \
    }

const contracts::ResourceClaim kClaims[] = {
    // 5 核心：任一失败即整体回滚（required/fail-closed）。
    CRAFT_CLAIM("hook:UIMix_ButtonInvenItemSelectExe", "F_UIMIX_BUTTON_INVEN_ITEM_SELECT_EXE_VMA",
                &place_execute, kGateKey, contracts::ResourceRequirement::kRequired,
                contracts::ResourceFailure::kFailClosed),
    CRAFT_CLAIM("hook:UIMix_ButtonMixingExe", "F_UIMIX_BUTTON_MIXING_EXE_VMA", &craft_execute,
                kGateKey, contracts::ResourceRequirement::kRequired,
                contracts::ResourceFailure::kFailClosed),
    CRAFT_CLAIM("hook:MIXSYSTEM_MakeItem", "F_MAKE_MIX_VMA", &make_item_execute, kGateKey,
                contracts::ResourceRequirement::kRequired,
                contracts::ResourceFailure::kFailClosed),
    CRAFT_CLAIM("hook:UIMix_ButtonMenuListExe", "F_UIMIX_BUTTON_MENU_LIST_EXE_VMA", &menu_execute,
                kGateKey, contracts::ResourceRequirement::kRequired,
                contracts::ResourceFailure::kFailClosed),
    CRAFT_CLAIM("hook:UIMix_ButtonRecipeExe", "F_UIMIX_BUTTON_RECIPE_EXE_VMA", &recipe_execute,
                kGateKey, contracts::ResourceRequirement::kRequired,
                contracts::ResourceFailure::kFailClosed),
    // 6 装饰（门控）：失败只 degraded、不拖垮核心链。
    CRAFT_CLAIM("hook:X_TEXTCTRL_SetTextControl", "F_XTEXTCTRL_SET_TEXT_CONTROL_VMA",
                &set_desc_text_execute, kGateKey, contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
    CRAFT_CLAIM("hook:ControlItem_Draw", "F_CONTROL_ITEM_DRAW_VMA", &control_item_draw_execute,
                kGateKey, contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
    CRAFT_CLAIM("hook:ITEM_DrawPorting", "F_ITEM_DRAW_PORTING_VMA", &item_draw_porting_execute,
                kGateKey, contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
    CRAFT_CLAIM("hook:UIMix_ButtonRecipeDraw", "F_UIMIX_BUTTON_RECIPE_DRAW_VMA",
                &button_recipe_draw_execute, kGateKey, contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
    CRAFT_CLAIM("hook:UIMix_Draw", "F_UIMIX_DRAW_VMA", &uimix_draw_execute, kGateKey,
                contracts::ResourceRequirement::kOptional, contracts::ResourceFailure::kFailSoft),
    CRAFT_CLAIM("hook:UIMix_ButtonMenuListDraw", "F_UIMIX_BUTTON_MENU_LIST_DRAW_VMA",
                &menu_list_draw_execute, kGateKey, contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
    // 常驻（gate=nullptr）：清空可能来自原版成功链，不该被别的开关左右。
    CRAFT_CLAIM("hook:UIMix_ResetStuffItemControl", "F_UIMIX_RESET_STUFF_ITEM_CONTROL_VMA",
                &reset_stuff_execute, nullptr, contracts::ResourceRequirement::kOptional,
                contracts::ResourceFailure::kFailSoft),
};
#undef CRAFT_CLAIM

constexpr uint32_t kClaimCount = sizeof(kClaims) / sizeof(kClaims[0]);

// optional_requires：能力缺失只置 degraded、不级联停用（保证 reset hook 仍工作）。
const contracts::CapabilityRequirement kOptionalRequires[] = {
    {contracts::kCraftResultCapabilityId,
     QolVersionRange{static_cast<uint16_t>(contracts::kCraftResultVersionMajor),
                     static_cast<uint16_t>(contracts::kCraftResultVersionMinor)}},
};

const contracts::CapabilityRequirement kRequires[] = {
    {contracts::kItemFactsCapabilityId,
     QolVersionRange{static_cast<uint16_t>(contracts::kItemFactsVersionMajor),
                      static_cast<uint16_t>(contracts::kItemFactsVersionMinor)}},
    {contracts::kCraftFactsCapabilityId,
     QolVersionRange{contracts::kCraftFactsVersion.major, contracts::kCraftFactsVersion.minor}},
};

const contracts::QolPortRequirement kPorts[] = {
    {contracts::QolPortId::QOL_PORT_CRAFT_UI_WRITE,
     {contracts::kQolPortCraftUiWriteVersion.major,
      contracts::kQolPortCraftUiWriteVersion.minor}},
};

int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.sdk == nullptr || ctx.sdk->capabilities.require == nullptr || ctx.ports == nullptr ||
        ctx.port_count != 1 || ctx.ports[0] == nullptr) {
        return 1;
    }
    const void* iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kItemFactsCapabilityId,
            {contracts::kItemFactsVersionMajor, contracts::kItemFactsVersionMinor}, &iface)) ||
        iface == nullptr || !contracts::item_facts_api_size_valid(
                                static_cast<const contracts::ItemFactsApi*>(iface))) {
        return 1;
    }
    set_item_facts(static_cast<const contracts::ItemFactsApi*>(iface));
    iface = nullptr;
    if (!contracts::succeeded(ctx.sdk->capabilities.require(
            contracts::kCraftFactsCapabilityId,
            {contracts::kCraftFactsVersion.major, contracts::kCraftFactsVersion.minor}, &iface)) ||
        iface == nullptr) {
        set_item_facts(nullptr);
        return 1;
    }
    set_craft_facts(static_cast<const contracts::CraftFactsApi*>(iface));
    const auto* write_port = static_cast<const contracts::QolCraftUiWritePort*>(ctx.ports[0]);
    if (!contracts::satisfies(write_port->abi_version,
                              {contracts::kQolPortCraftUiWriteVersion.major,
                               contracts::kQolPortCraftUiWriteVersion.minor})) {
        set_item_facts(nullptr);
        set_craft_facts(nullptr);
        return 1;
    }
    set_write_port(write_port);
    set_context(ctx);
    // 自身初始化无额外动作：资源由宿主声明式安装，backup 经 on_resource_ready 交还。
    return 0;
}

// 空操作幂等：资源物理释放由宿主逆序执行，本插件无额外状态需要清理。
void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    set_item_facts(nullptr);
    set_craft_facts(nullptr);
    set_write_port(nullptr);
}
void plugin_on_enabled(contracts::PluginContext& ctx) { (void)ctx; }
void plugin_on_disabled(contracts::PluginContext& ctx) { (void)ctx; }

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = true;
    out.error = nullptr;
}

int32_t on_resource_ready(contracts::PluginContext& ctx, const char* resource_id, void* backup) {
    if (ctx.sdk == nullptr) return 1;
    set_context(ctx);
    if (resource_id == nullptr) return 1;
    if (std::strcmp(resource_id, "hook:UIMix_ButtonInvenItemSelectExe") == 0) {
        g_backup_place = reinterpret_cast<UIMixButtonInvenItemSelectExeFn>(backup);
    } else if (std::strcmp(resource_id, "hook:UIMix_ButtonMixingExe") == 0) {
        g_backup_mixing = reinterpret_cast<UIMixButtonMixingExeFn>(backup);
    } else if (std::strcmp(resource_id, "hook:MIXSYSTEM_MakeItem") == 0) {
        g_backup_make_item = reinterpret_cast<MakeMixFn>(backup);
    } else if (std::strcmp(resource_id, "hook:UIMix_ButtonMenuListExe") == 0) {
        g_backup_menu = reinterpret_cast<UIMixButtonMenuListExeFn>(backup);
    } else if (std::strcmp(resource_id, "hook:UIMix_ButtonRecipeExe") == 0) {
        g_backup_recipe = reinterpret_cast<UIMixButtonRecipeExeFn>(backup);
    } else if (std::strcmp(resource_id, "hook:X_TEXTCTRL_SetTextControl") == 0) {
        g_backup_set_desc_text = reinterpret_cast<XTextCtrlSetTextControlFn>(backup);
    } else if (std::strcmp(resource_id, "hook:ControlItem_Draw") == 0) {
        g_backup_control_item_draw = reinterpret_cast<ControlItemDrawFn>(backup);
    } else if (std::strcmp(resource_id, "hook:ITEM_DrawPorting") == 0) {
        g_backup_item_draw_porting = reinterpret_cast<ItemDrawPortingFn>(backup);
    } else if (std::strcmp(resource_id, "hook:UIMix_ButtonRecipeDraw") == 0) {
        g_backup_button_recipe_draw = reinterpret_cast<UIMixButtonRecipeDrawFn>(backup);
    } else if (std::strcmp(resource_id, "hook:UIMix_Draw") == 0) {
        g_backup_uimix_draw = reinterpret_cast<UIMixDrawFn>(backup);
    } else if (std::strcmp(resource_id, "hook:UIMix_ButtonMenuListDraw") == 0) {
        g_backup_menu_list_draw = reinterpret_cast<UIMixButtonMenuListDrawFn>(backup);
    } else if (std::strcmp(resource_id, "hook:UIMix_ResetStuffItemControl") == 0) {
        g_backup_reset_stuff = reinterpret_cast<UIMixResetStuffItemControlFn>(backup);
    } else {
        return 1;  // 未知资源：fail-closed（宿主按 resource_ready_failed 回滚）
    }
    return 0;
}

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    static const contracts::PluginDescriptor descriptor = {
        kPluginId,
        kPluginVersion,
        2, kRequires,  // requires=item.facts + craft.facts
        1, kOptionalRequires,  // optional_requires=[craft_result]
        0, nullptr,  // provides
        kClaimCount, kClaims,
        0, nullptr,  // jni
        &plugin_install,
        &plugin_deactivate,
        &plugin_on_enabled,
        &plugin_on_disabled,
        &plugin_fill_status,
        &on_resource_ready,
        kPorts,
        1,
    };
    return descriptor;
}

}  // namespace qol::features::craft_ui
