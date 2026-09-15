#include "game_ui_custom_recipe.h"

#include "custom_recipe_catalog.h"
#include "custom_recipe_rules.h"
#include "custom_recipe_table.h"

#include "feature/attribute_range/attribute_range.h"
#include "feature/attribute_range/game_ui_attr_range.h"
#include "feature/patch/native_inventory_hook.h"
#include "core/native/qol_log.h"
#include "game_access.h"
#include "game_symbols.h"

#include <atomic>
#include <cstdint>

namespace {

// LSPosed native hook 写入的原函数指针（call_orig）。
UIMixButtonInvenItemSelectExeFn g_backup_place = nullptr;
UIMixButtonMixingExeFn g_backup_mixing = nullptr;
MakeMixFn g_backup_make_item = nullptr;
UIMixButtonMenuListExeFn g_backup_menu = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_attempted{false};
std::atomic<bool> g_custom_recipe_enabled{false};

// 真机验收用的有界诊断日志：每次进程最多 16 条，避免放料/合成高频刷屏。
std::atomic<int> g_verify_log_budget{16};

// 原生提示文本项（§3.7 复用既有串，不新增本地化）。
constexpr uint32_t kTextOnlyJewel = 97;   // 「只有宝石道具才可以。」
constexpr uint32_t kTextMaxCrafted = 105; // 「已经达到合成最大值，无法再次进行合成。」

// 主角在队伍成员数组中的下标（PARTY_GetMember 入参；0 = 主角）。
constexpr int kPartyLeaderMemberIndex = 0;

inline bool enabled() { return g_custom_recipe_enabled.load(std::memory_order_acquire); }

// 当前 mixType（[g_uimix+0x48]）。
uint32_t current_mix_type() {
    if (g_uimix == nullptr) return 0;
    return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_MIXTYPE);
}

// 目标槽 ControlItem 指针（[g_uimix+0x40]）。
void* target_slot_control() {
    if (g_uimix == nullptr) return nullptr;
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_TARGET_ITEM);
}

// 目标槽当前物品（ControlItem_GetItem([+0x40])）。
void* target_slot_item() {
    void* ctrl = target_slot_control();
    if (ctrl == nullptr || fn_control_item_get_item == nullptr) return nullptr;
    return fn_control_item_get_item(ctrl);
}

// 物品是否独立宝石（类别 = item+I_TYPE u16 的 bits6-15；见 game_symbols I_TYPE_CATEGORY_*）。
bool item_is_jewel(void* item) {
    if (item == nullptr || fn_is_jewel == nullptr) return false;
    const uint16_t type_flags =
        *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(item) + I_TYPE);
    const int category =
        static_cast<int>((type_flags >> I_TYPE_CATEGORY_SHIFT) & I_TYPE_CATEGORY_MASK);
    return fn_is_jewel(category) != 0;
}

// 读宝石数值位域并按 attr_range classify 判定当前档；探测失败返回 false。
bool jewel_tier(void* item, attr_range::Tier* out_tier, int* out_value, int* out_min,
                int* out_max) {
    if (item == nullptr || out_tier == nullptr) return false;
    const uint32_t word =
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(item) + I_JEWEL_VALUE_WORD);
    const int type = static_cast<int>((word >> JEWEL_TYPE_SHIFT) & JEWEL_TYPE_MASK);
    const int value = static_cast<int>(word & JEWEL_VALUE_MASK);
    int min = 0;
    int max = 0;
    if (!attr_range_probe_jewel_range(type, item, &min, &max)) return false;
    *out_tier = attr_range::classify(value, min, max);
    if (out_value != nullptr) *out_value = value;
    if (out_min != nullptr) *out_min = min;
    if (out_max != nullptr) *out_max = max;
    return true;
}

// 当前主角等级（PARTY_GetMember(0) + C_LEVEL int8；读法同 game_system.cpp:61）。
// 读不到时返回 0，等价于「1 级以下」即不减免材料（保守），不会出现意外的免费合成。
int current_character_level() {
    if (fn_get_member == nullptr) return 0;
    void* member = fn_get_member(kPartyLeaderMemberIndex);
    if (member == nullptr) return 0;
    return static_cast<int>(*reinterpret_cast<int8_t*>(static_cast<uint8_t*>(member) + C_LEVEL));
}

// 按 Def::count_rule 重算材料需求数，写入 stuffList 条目 +6（u16）。
// 该字段是扣料（UseStuff 读 +6，为 0 直接跳过）与显示（UIMix_Draw 每帧读 +6 画「持有/需求」）
// 的唯一真源，故改这一处即同时生效，无需调用任何刷新函数（§3.9）。
// 档位无法识别（不在 28..32）时直接返回、保留原需求数，避免被误判成 0 消耗。
void apply_material_count(const custom_recipe::Def* def, void* jewel) {
    if (def == nullptr || jewel == nullptr || g_uimix == nullptr) return;
    if (def->count_rule != custom_recipe::CountRule::kJewelGradeAndLevel) return;
    if (def->material_count == 0) return;

    const uint16_t type_flags =
        *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(jewel) + I_TYPE);
    const int grade = custom_recipe::jewel_grade_from_category(
        static_cast<int>((type_flags >> I_TYPE_CATEGORY_SHIFT) & I_TYPE_CATEGORY_MASK));
    if (grade < 1) return;

    auto* stuff_list =
        *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_STUFF_LIST);
    const uint32_t slots =
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_STUFF_NUM);
    if (stuff_list == nullptr) return;

    const int level = current_character_level();
    for (uint8_t i = 0; i < def->material_count && static_cast<uint32_t>(i) < slots; ++i) {
        const int want = custom_recipe::material_count_for(def->materials[i].count, grade, level);
        *reinterpret_cast<uint16_t*>(stuff_list +
                                     static_cast<size_t>(i) * UIMIX_STUFF_ENTRY_SIZE +
                                     UIMIX_STUFF_ENTRY_NEED) = static_cast<uint16_t>(want);
    }
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "material need grade=%d level=%d base=%d need=%d",
                      grade, level, def->materials[0].count,
                      custom_recipe::material_count_for(def->materials[0].count, grade, level));
    }
}

void show_text_data(uint32_t word_id) {
    if (fn_popup_create_ok_from_textdata != nullptr) {
        fn_popup_create_ok_from_textdata(word_id, 0, 0, 0);
    }
}

// 类型/菜单按钮 hook（§4.3）：点「混沌合成」页签时，本函数随即执行
// SetType(3) → MIXSYSTEM_CreateRecipeList → 建配方按钮。注入必须早于建按钮，
// 故在此处（call_orig 之前）确保注入完成，使首次点开页签即能看到注入配方。
void custom_menu_wrapper(void* ctrl) {
    if (enabled()) {
        custom_recipe::custom_recipe_table_ensure();
    }
    if (g_backup_menu != nullptr) {
        g_backup_menu(ctrl);
    }
}

// 放料 hook（§4.4）：先转调原函数，再纠正非法宝石 + 强制费用 0。
void custom_place_wrapper(void* ctrl) {
    if (g_backup_place == nullptr) return;
    // def 非空即代表「当前选中的是模块注入的配方」，与总开关无关
    //（停用后旧面板仍可能持有它，必须能识别以 fail-closed）。
    const custom_recipe::Def* def = custom_recipe::def_for_mix_type(current_mix_type());
    if (def == nullptr) {
        g_backup_place(ctrl);
        return;
    }
    if (!enabled()) {
        // 停用后残留的旧面板仍可能指向注入配方：原版会照常写入目标槽并算费用，
        // 且合成时会走原版通用路径白扣材料 —— 故转调后立即撤销放置。
        g_backup_place(ctrl);
        void* slot = target_slot_control();
        if (slot != nullptr && fn_control_item_set_item != nullptr) {
            fn_control_item_set_item(slot, nullptr);
        }
        return;
    }
    if (!custom_recipe::custom_recipe_table_ensure()) {
        g_backup_place(ctrl);
        return;
    }
    g_backup_place(ctrl);  // 原版：CheckMixture（注入 mixType 恒返回 0）→ SetItem → 写费用

    void* slot_ctrl = target_slot_control();
    if (slot_ctrl == nullptr) return;
    void* item = target_slot_item();
    if (item == nullptr) return;  // 本次未放入任何物品
    if (!item_is_jewel(item)) {
        // 非宝石：撤销放置 + 原生提示。
        if (fn_control_item_set_item != nullptr) {
            fn_control_item_set_item(slot_ctrl, nullptr);
        }
        show_text_data(kTextOnlyJewel);
        return;
    }
    // 宝石：按宝石档位 + 角色等级写入材料需求数（下一帧绘制即显示），再强制费用 0
    //（覆盖原函数 CAL_Calculate 结果，显示与实扣同时为 0）。
    apply_material_count(def, item);
    *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_COST) = 0;
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "place accepted jewel mixType=%u cost=0",
                      current_mix_type());
    }
}

// 合成按钮 hook（§4.5）：前置校验（非宝石弹 97 / 已金档弹 105 并拦截），否则转调原函数。
void custom_mixing_wrapper(void* ctrl) {
    if (g_backup_mixing == nullptr) return;
    const custom_recipe::Def* def = custom_recipe::def_for_mix_type(current_mix_type());
    if (def == nullptr) {
        g_backup_mixing(ctrl);
        return;
    }
    if (!enabled() || !custom_recipe::custom_recipe_table_ensure()) {
        // fail-closed：不允许经残留面板对该 mixType 发起合成。原版会走通用
        // CreatePerfectItem 路径：产物无意义、材料与费用照扣。宁可无响应也不白扣。
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "craft blocked reason=disabled mixType=%u",
                     current_mix_type());
        return;
    }
    void* item = target_slot_item();
    if (item == nullptr || !item_is_jewel(item)) {
        show_text_data(kTextOnlyJewel);
        return;
    }
    attr_range::Tier tier = attr_range::Tier::Grey;
    if (jewel_tier(item, &tier, nullptr, nullptr, nullptr) && tier == attr_range::Tier::Gold) {
        show_text_data(kTextMaxCrafted);
        return;
    }
    g_backup_mixing(ctrl);  // 费用 0 时原版自动跳过金币校验并弹确认框 → StartMix
}

// 产物 hook（§4.6）：自定义 mixType 按 Def.kind 分派，读改写宝石 bits0-10；其余转调原函数。
int custom_make_item_wrapper(int32_t mix_type, void** out_item) {
    const custom_recipe::Def* def =
        custom_recipe::def_for_mix_type(static_cast<uint32_t>(mix_type));
    if (def == nullptr) {
        return (g_backup_make_item != nullptr) ? g_backup_make_item(mix_type, out_item) : 1;
    }
    // fail-closed：停用期间绝不回落到原版 —— 注入记录号对原表是越界下标，
    // 原版通用路径会用它去读 RECIPEBASE 之外的字节。
    if (!enabled()) return 1;
    if (def->kind != custom_recipe::Kind::kJewelTierUp) {
        return (g_backup_make_item != nullptr) ? g_backup_make_item(mix_type, out_item) : 1;
    }
    if (out_item == nullptr || *out_item == nullptr || fn_math_get_random == nullptr) {
        return 1;  // 失败兜底：不扣料、不扣钱、不清槽
    }
    void* item = *out_item;
    // 扣料数量定稿：StartMix 在 MakeItem 返回 0 之后紧接着调 UseStuff 读 stuffList，
    // 此处写入的 +6 即最终实际扣除量（与放料/确认框处写入同源同式，保证一致）。
    apply_material_count(def, item);
    uint32_t* word_ptr =
        reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(item) + I_JEWEL_VALUE_WORD);
    const uint32_t word = *word_ptr;
    const int type = static_cast<int>((word >> JEWEL_TYPE_SHIFT) & JEWEL_TYPE_MASK);
    const int value = static_cast<int>(word & JEWEL_VALUE_MASK);
    int min = 0;
    int max = 0;
    if (!attr_range_probe_jewel_range(type, item, &min, &max)) {
        return 1;
    }
    attr_range::Tier target = attr_range::Tier::Grey;
    int new_value = 0;
    const custom_recipe::UpgradeResult result = custom_recipe::compute_tier_up_value(
        value, min, max, fn_math_get_random, &target, &new_value);
    if (result != custom_recipe::UpgradeResult::kOk) {
        return 1;  // 金档 / 退化 → 失败兜底（与 §4.5 前置拦截互为双保险）
    }
    // read-modify-write：仅改 bits0-10，保留 bits11-17（等级）与 bits18-23（属性类型）。
    *word_ptr = (word & ~static_cast<uint32_t>(JEWEL_VALUE_MASK)) |
                (static_cast<uint32_t>(new_value) & JEWEL_VALUE_MASK);
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe,
                      "make item tier-up type=%d value=%d->%d range=[%d,%d] target=%d", type,
                      value, new_value, min, max, custom_recipe::tier_ordinal(target));
    }
    return 0;
}

bool install_one(NativeHookFunType hook, uintptr_t target, void* replacement, void** backup,
                 const char* name) {
    const int rc = hook(reinterpret_cast<void*>(target), replacement, backup);
    if (rc != 0 || backup == nullptr || *backup == nullptr) {
        QOL_LOG_ERROR(QolDomain::kCustomRecipe, "%s hook failed rc=%d", name, rc);
        return false;
    }
    return true;
}

}  // namespace

bool set_custom_recipe_enabled(bool enabled_value) {
    g_custom_recipe_enabled.store(enabled_value, std::memory_order_release);
    if (!enabled_value) {
        // 停用：收回注入记录数，使任何配方查询都不再命中注入记录（保留模块缓冲作为表体，
        // 避免面板残留数组里的注入下标越界读）。再次启用时 ensure() 会把记录数归位。
        custom_recipe::custom_recipe_table_deactivate();
        return true;
    }
    if (!custom_recipe_ui_install_if_ready()) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "install deferred reason=bridge_or_hook_not_ready");
        return true;
    }
    if (!custom_recipe::custom_recipe_table_ensure()) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "table inject deferred reason=table_not_loaded");
    }
    return true;
}

bool custom_recipe_enabled() { return enabled(); }

bool custom_recipe_ui_install_if_ready() {
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

    if (!install_one(hook, place, reinterpret_cast<void*>(&custom_place_wrapper),
                     reinterpret_cast<void**>(&g_backup_place),
                     "UIMix_ButtonInvenItemSelectExe") ||
        !install_one(hook, mixing, reinterpret_cast<void*>(&custom_mixing_wrapper),
                     reinterpret_cast<void**>(&g_backup_mixing), "UIMix_ButtonMixingExe") ||
        !install_one(hook, make_item, reinterpret_cast<void*>(&custom_make_item_wrapper),
                     reinterpret_cast<void**>(&g_backup_make_item), "MIXSYSTEM_MakeItem") ||
        !install_one(hook, menu, reinterpret_cast<void*>(&custom_menu_wrapper),
                     reinterpret_cast<void**>(&g_backup_menu), "UIMix_ButtonMenuListExe")) {
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kCustomRecipe, "custom recipe hooks installed");
    return true;
}
