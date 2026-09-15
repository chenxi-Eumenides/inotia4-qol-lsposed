#include "custom_recipe_api.h"

#include "custom_recipe_catalog.h"
#include "custom_recipe_rules.h"
#include "custom_recipe_table.h"

#include "feature/craft_ui/craft_ui.h"
#include "feature/craft_ui/craft_ui_hooks.h"

#include "core/native/save_enter.h"

#include "data/native/item_class.h"
#include "feature/attribute_range/attribute_range.h"
#include "feature/attribute_range/game_ui_attr_range.h"
#include "feature/patch/native_inventory_hook.h"
#include "feature/ui/module_text.h"
#include "core/native/qol_log.h"
#include "game_access.h"
#include "game_symbols.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace {

std::atomic<bool> g_custom_recipe_enabled{false};

// 真机验收用的有界诊断日志：每次进程最多 16 条，避免放料/合成高频刷屏。
std::atomic<int> g_verify_log_budget{16};

constexpr size_t kThreeSlotStuffCount = 3;

// 主角在队伍成员数组中的下标（PARTY_GetMember 入参；0 = 主角）。
constexpr int kPartyLeaderMemberIndex = 0;

// Form → 借用的原版 type（UIMIX_SLOT_TYPE 语义：0=药水 1=宝石 2=打孔 3=混沌 4=传说）。
// 绝不 SetType(>=5)：原版 ResetActiveControl 对 type>=5 走未初始化寄存器路径。
constexpr int64_t kFormTypeMultiInputCreate = 1;  // 填入多个物品 → 生成
constexpr int64_t kFormTypeTargetAndCreate = 3;   // 填入一个物品 + 消耗材料 → 生成/原地修改
constexpr int64_t kFormTypeConsumeToCreate = 4;   // 直接消耗材料生成物品


// 放料 / 合成按钮 / 产物三个 hook 的统一介入判定：当前 mixType 是模块配方且
// kind == kJewelTierUp（与总开关无关，停用后仍需识别以 fail-closed）。
// kNativePassThrough（宝石强化页的「合成」入口）返回 nullptr → 三个 wrapper 一律转调原函数：
// 其放料校验、费用与产物全部由原版 + gemcraft 承担（gemcraft 合成时把 [+0x48] 改写回
// 12+(category-28)），模块若介入（撤销放置 / 强制费用 0 / 改写产物）会破坏 3:1 宝石合成流程。
const custom_recipe::Def* module_recipe_for_mix_type(uint32_t mix_type) {
    const custom_recipe::Def* def = custom_recipe::def_for_mix_type(mix_type);
    if (def == nullptr || def->kind != custom_recipe::Kind::kJewelTierUp) return nullptr;
    return def;
}

// 物品类别 = `item + I_TYPE`(u16) 的 bits6-15；对本项目用到的物品类别而言 == itemId
//（设计册 §2.8：文档类别表与 itemId 区间一致，`ITEMDATABASE` 记录内无独立类别字段）。
// 空物品返回 0 = 「该槽为空」的匹配键。
uint16_t item_category(void* item) {
    if (item == nullptr) return 0;
    const uint16_t type_flags =
        *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(item) + I_TYPE);
    return static_cast<uint16_t>((type_flags >> I_TYPE_CATEGORY_SHIFT) & I_TYPE_CATEGORY_MASK);
}

// 物品是否独立宝石（类别 = item+I_TYPE u16 的 bits6-15；见 game_symbols I_TYPE_CATEGORY_*）。
// 是否可堆叠（ITEMCLASSBASE[category*stride+6] bit0；口径与 data/native/item_class.h 一致）。
bool item_is_stackable(void* item) {
    return item_count_encoding(item) == stack_codec::CountEncoding::kEncoded;
}

// 某类别当前持有总数：**用游戏自带的 INVEN_GetItemCount(category)**（0x104260「原版类别数量」，
// MakeStuffSlot 填材料格「持有」数用的就是它）。读不到时返回 0（fail-closed）。
int category_held_count(uint16_t category) {
    if (fn_inven_get_item_count == nullptr) return 0;
    const int n = fn_inven_get_item_count(static_cast<int32_t>(category));
    return n > 0 ? n : 0;
}

bool item_is_jewel(void* item) {
    if (item == nullptr || fn_is_jewel == nullptr) return false;
    return fn_is_jewel(static_cast<int>(item_category(item))) != 0;
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

// ---- UIMix 槽访问与 Form 分派辅助（配方点击 hook 用）----

// Form → 借用的原版 type（0..4）。三枚举全覆盖，末行仅防御。
int64_t form_borrowed_type(custom_recipe::Form form) {
    switch (form) {
        case custom_recipe::Form::kConsumeToCreate: return kFormTypeConsumeToCreate;
        case custom_recipe::Form::kTargetAndCreate: return kFormTypeTargetAndCreate;
        case custom_recipe::Form::kMultiInputCreate: return kFormTypeMultiInputCreate;
    }
    return kFormTypeTargetAndCreate;
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

// ---- 3 格隐式配方（宝石强化页 UIMix type 1；§2.8 需求 / §4.12 落位）----
//
// 页面形态借用原版 type 1：3 个填入格 = `[g_uimix+0xc8]` 组的前 3 个子控件（必须经
// `ControlObject_GetChild@0x9eacc` 取，**不是** `+0xc8+i*8` 直索引），选中填入格下标在
// `[+0x128]`（i64，-1=未选）。读格用 `ControlItem_GetItem@0xaada8`、写格用
// `ControlItem_SetItem@0xaad60`（裸指针写，空 = 0）。
//
// 放料**不转调原函数**：原版 type 1 放料段（`UIMix_ButtonInvenItemSelectExe` 的 0xc23bc）有三道
// 校验（IsJewel 弹 97 / 与 stuffList[0] 档位比对弹 98 / 重复放置弹 99），而本模式要求「任意物品
// 均可填入」；三道校验的失败路径只有弹窗、无不可撤销副作用，故模块自行实现放料即可。
//
// 合成**不转调原函数**：原版 type 1 合成段（`UIMix_ButtonMixingExe` 的 0xc2214）完全不读 3 格
// 内容，只做费用/金币检查后弹 YesNo（确认文本 18），回调 = `[0x2f5e78]`（`UIMix_StartMix`）。
// 本模式改为「先查表、命中才弹同一个 YesNo、回调换成模块自己的产出/扣料流程」；未命中直接弹
// 94 并返回 ⇒ 拒绝路径既不进原版合成、也不弹确认框，材料/金币分文不动（本模式无费用）。

// 待合成的命中条目（YesNo 回调与放料/合成同在游戏 UI 主线程，无需加锁）。
// 回调经 x3 传入、上下文只能用模块自有全局（x6/param 会被原版当钱数渲染，见 game_symbols
// F_UIPOPUPMSG_CREATE_YESNO_FROM_TEXTDATA_VMA 注释）。
const custom_recipe::ThreeSlotRecipe* g_pending_three_slot = nullptr;

// 3 格模式的介入判定：总开关启用 + 当前页面为宝石强化页（type 1）+ `[+0x48]` 命中
// kThreeSlotCraft。停用后（含表已收回记录数的残留面板）一律回落原版行为（VM-D7）。
const custom_recipe::Def* three_slot_def_for_current() {
    if (!custom_recipe::enabled()) return nullptr;
    if (g_uimix == nullptr || fn_uimix_get_type == nullptr || fn_uimix_get_type() != 1) {
        return nullptr;
    }
    const custom_recipe::Def* def = custom_recipe::def_for_mix_type(craft_ui::mix_type());
    if (def == nullptr || def->kind != custom_recipe::Kind::kThreeSlotCraft) return nullptr;
    return def;
}

// `kAnySpecialEquipSlot` 的判定谓词：类别是否带 ITEMCLASSBASE 记录 +7 bit4（NPC 专属保护）。
// 与 feature/special_equip 放行的是同一组 26 条；这里经 data 层原语判定，不跨 feature 依赖。
// 表未就绪时 data 层 fail-closed 返回 false → 相关配方不命中，不会误合成。
bool is_special_equip_category(uint16_t category) {
    return category_is_no_equip(static_cast<int>(category));
}

}  // namespace

namespace custom_recipe {

bool set_enabled(bool enabled_value) {
    g_custom_recipe_enabled.store(enabled_value, std::memory_order_release);
    if (!enabled_value) {
        // 停用：收回注入记录数，使任何配方查询都不再命中注入记录（保留模块缓冲作为表体，
        // 避免面板残留数组里的注入下标越界读）。再次启用时 ensure() 会把记录数归位。
        custom_recipe::custom_recipe_table_deactivate();
        return true;
    }
    if (!craft_ui_install_if_ready()) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "install deferred reason=bridge_or_hook_not_ready");
        return true;
    }
    if (!custom_recipe::custom_recipe_table_ensure()) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "table inject deferred reason=table_not_loaded");
    }
    return true;
}

bool enabled() { return g_custom_recipe_enabled.load(std::memory_order_acquire); }

// 当前所选配方是否为「3 格隐式配方」模式（供其它 feature 判断是否让路，§4.12）。
// 只按 `[g_uimix+0x48]` → 目录 kind 判定，**不含总开关**：停用后表记录数收回，
// `def_for_mix_type` 自然不再命中注入记录，因此本函数返回 false。
bool three_slot_mode_active() {
    if (g_uimix == nullptr) return false;
    const custom_recipe::Def* def = custom_recipe::def_for_mix_type(craft_ui::mix_type());
    return def != nullptr && def->kind == custom_recipe::Kind::kThreeSlotCraft;
}

// ---------------------------------------------------------------------------
// 动态特殊装备配方：进档随机重建
// ---------------------------------------------------------------------------

namespace {

// 进入存档（读档 / 新档，world 就绪）后触发一次：为每件特殊装备重新抽 3 个互不相同的材料。
// 随机源用游戏的 MATH_GetRandom（与产物掷值同源）；符号未就绪时 build 返回 0 →
// 动态表被清空（fail-closed：宁可没有这条配方，也不留上一档的过期表）。
void refresh_dynamic_recipes(void* /*ctx*/) {
    custom_recipe::ThreeSlotRecipe buffer[custom_recipe::kMaxDynamicRecipes];
    const size_t n = custom_recipe::build_dynamic_recipes(
        custom_recipe::kSpecialEquipCategories, custom_recipe::kSpecialEquipCategoryCount,
        custom_recipe::kMaterialPool, custom_recipe::kMaterialPoolSize, fn_math_get_random, buffer,
        custom_recipe::kMaxDynamicRecipes);
    custom_recipe::set_dynamic_three_slot_recipes(buffer, n);
    QOL_LOG_INFO(QolDomain::kCustomRecipe,
                 "dynamic special recipes rebuilt count=%zu equip=%zu materialPool=%zu",
                 n, custom_recipe::kSpecialEquipCategoryCount, custom_recipe::kMaterialPoolSize);
    // 逐条映射刻意不打印（用户裁决「无信息」；临时排障日志已于开发收尾时删除）。
}

}  // namespace

void register_dynamic_recipes() {
    // save_enter_register 对同一 fn+ctx 幂等，重复调用不会叠加。
    save_enter_register(&refresh_dynamic_recipes, nullptr);
}

// ===========================================================================
// 结果层对外接口（界面层 feature/craft_ui 单向调用）
// ===========================================================================
//
// 约定（用户裁定 2026-09-16：界面归界面，结果归结果）：本层**不认识界面** ——
// 不弹窗、不改选中格、不碰控件、不播声音；只回传结果枚举与产物/扣料决策。
// 弹什么文案、清不清空填入格、选哪个格，全部由 craft_ui 按枚举决定。
//
// 允许的单向依赖：本层可以调 craft_ui 的**只读/纯状态**原语（读格、读 mixType、读选中格）。

namespace {

// 原版配方模板「用3个$S%s$B合成一个$R%s$B。」的 UTF-8 前缀「用3个」。
bool desc_text_matches_recipe_template(const char* text) {
    static constexpr uint8_t kPrefix[] = {0xE7, 0x94, 0xA8, 0x33, 0xE4, 0xB8, 0xAA};
    if (text == nullptr) return false;
    for (size_t i = 0; i < sizeof(kPrefix); ++i) {
        if (static_cast<uint8_t>(text[i]) != kPrefix[i]) return false;
    }
    return true;
}

constexpr size_t kDescTextScanMax = 512;  // 描述文本扫描上限（保守上界，仅用于求串长）

// 有界求串长（不越界扫描）。
size_t text_length_bounded(const char* text, size_t max) {
    if (text == nullptr) return 0;
    size_t n = 0;
    while (n < max && text[n] != '\0') ++n;
    return n;
}

constexpr int64_t kNoBorrowedType = -1;

}  // namespace

bool is_jewel_item(void* item) { return item_is_jewel(item); }

void apply_material_count_for(const Def* def, void* jewel) { apply_material_count(def, jewel); }

int64_t form_ui_type_for_mix_type(uint32_t mix_type) {
    const Def* def = def_for_mix_type(mix_type);
    if (def == nullptr) return kNoBorrowedType;
    return form_borrowed_type(def->form);
}

bool desc_should_replace(const char* text) {
    // 门控（按**内容**而非指针，见设计册 §7.28 的真机日志定案）：
    //   ① 当前 [+0x48] 命中模块记录 ② 文本以原版模板前缀「用3个」开头
    //   ③ 原文本不短于模块文案（**只做缩短替换**，绝不放长，避免越界写堆缓冲）。
    if (g_base == 0 || def_for_mix_type(craft_ui::mix_type()) == nullptr) return false;
    if (!desc_text_matches_recipe_template(text)) return false;
    return text_length_bounded(text, kDescTextScanMax) >= (sizeof(kModuleRecipeDesc) - 1);
}

CraftGate craft_gate() {
    void* item = craft_ui::target_slot_item();
    if (item == nullptr || !item_is_jewel(item)) return CraftGate::kNotJewel;
    attr_range::Tier tier = attr_range::Tier::Grey;
    if (jewel_tier(item, &tier, nullptr, nullptr, nullptr) && tier == attr_range::Tier::Gold) {
        return CraftGate::kAlreadyGold;
    }
    return CraftGate::kPass;
}

PlaceOutcome three_slot_place(int64_t slot_index) {
    if (!three_slot_mode_active()) return PlaceOutcome::kNotMine;
    void* item = craft_ui::selected_inven_item();
    const bool valid_slot =
        slot_index >= 0 && static_cast<size_t>(slot_index) < kThreeSlotStuffCount;
    if (item == nullptr || !valid_slot) return PlaceOutcome::kPlaced;  // 无事发生

    //   · 不可堆叠物品（如宝石）= 1 个对象：同一对象不得占两格 → kAlreadyPlaced（界面弹 99）；
    //   · 可堆叠物品 = 1 个单位：同一个堆**可以**占多格（重复点添加），但受库存约束
    //     「已占该堆的格数 + 1 ≤ 该堆当前数量」⇒「2 个物品不能添加 3 次」，超限 → kNotEnoughHeld。
    int placed_same = 0;
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        if (static_cast<int64_t>(i) == slot_index) continue;
        if (craft_ui::stuff_item(i) == item) ++placed_same;
    }
    if (placed_same > 0 && !item_is_stackable(item)) return PlaceOutcome::kAlreadyPlaced;

    if (item_is_stackable(item)) {
        // 上限用**游戏自带的类别持有总数** INVEN_GetItemCount(category) —— 与扣料
        // INVEN_RemoveItemData(category,1) 口径一致。
        const uint16_t category = item_category(item);
        int same_category_placed = 0;
        for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
            if (static_cast<int64_t>(i) == slot_index) continue;
            void* other = craft_ui::stuff_item(i);
            if (other != nullptr && item_category(other) == category) ++same_category_placed;
        }
        if (!slot_add_allowed(same_category_placed, category_held_count(category))) {
            return PlaceOutcome::kNotEnoughHeld;
        }
    }

    void* slot = craft_ui::stuff_slot_control(static_cast<size_t>(slot_index));
    if (slot == nullptr || fn_control_item_set_item == nullptr) return PlaceOutcome::kPlaced;
    if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();  // 与原版放料前一致（0xc2364）
    fn_control_item_set_item(slot, item);
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "three slot place idx=%lld category=%u",
                      static_cast<long long>(slot_index),
                      static_cast<uint32_t>(item_category(item)));
    }
    return PlaceOutcome::kPlaced;
}

PrepareOutcome three_slot_prepare() {
    if (!three_slot_mode_active()) return PrepareOutcome::kNotMine;
    uint16_t categories[kThreeSlotStuffCount] = {0, 0, 0};
    craft_ui::read_stuff_categories(categories);
    const ThreeSlotRecipe* recipe = match_three_slot(categories, &is_special_equip_category);
    if (recipe == nullptr) {
        if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
            QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "three slot miss slots=%u,%u,%u", categories[0],
                          categories[1], categories[2]);
        }
        return PrepareOutcome::kNoMatch;
    }
    g_pending_three_slot = recipe;  // 待 YesNo 确认；确认框由界面层弹
    return PrepareOutcome::kConfirmPending;
}

CraftOutcome three_slot_execute() {
    const ThreeSlotRecipe* recipe = g_pending_three_slot;
    g_pending_three_slot = nullptr;
    if (recipe == nullptr) return CraftOutcome::kStaleSlots;  // 重复回调/已被消费：按「格子已变」处理

    // 回调前重新读格：确认框停留期间用户可能改动填入格，一律以回调时刻的格子内容为准。
    void* items[kThreeSlotStuffCount] = {nullptr, nullptr, nullptr};
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        items[i] = craft_ui::stuff_item(i);
    }
    // 产物 = 第 1 格物品自身（kScaleFirstItem / kMaxSocketEnchantFirstItem）时，必须在**扣料前**
    // 抓取源物品的宝石字与类别：扣料会销毁该对象（不可堆叠 → INVEN_RemoveItem 整堆删），
    // 此后指针失效。
    uint16_t product_category = recipe->product;
    uint32_t source_jewel_word = 0;
    bool has_source_jewel = false;
    if (recipe->product_mode == ProductMode::kScaleFirstItem) {
        if (items[0] == nullptr || !item_is_jewel(items[0])) {
            return CraftOutcome::kStaleSlots;  // 第 1 格被换成非宝石 → 不消耗、不产出
        }
        product_category = item_category(items[0]);
        source_jewel_word = *reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(items[0]) + I_JEWEL_VALUE_WORD);
        has_source_jewel = true;
    } else if (recipe->product_mode == ProductMode::kMaxSocketEnchantFirstItem) {
        if (items[0] == nullptr) return CraftOutcome::kStaleSlots;
        product_category = item_category(items[0]);
    }
    // **扣料前必须复核库存（fail-closed）**：同一堆要扣的单位数（= 引用它的格数）大于其当前
    // 数量时 → 中止，不消耗任何材料、不产出。
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        void* it = items[i];
        if (it == nullptr || !item_is_stackable(it)) continue;
        const uint16_t category = item_category(it);
        int units = 0;
        for (size_t j = 0; j < kThreeSlotStuffCount; ++j) {
            if (items[j] != nullptr && item_category(items[j]) == category) ++units;
        }
        if (!stack_units_available(units, category_held_count(category))) {
            return CraftOutcome::kNotEnoughHeld;
        }
    }
    // 扣料分两类：不可堆叠槽 → INVEN_RemoveItem（按对象整堆删，已 hook，扩展袋物品同样可删）；
    // 可堆叠槽 → INVEN_RemoveItemData(category, 1)（**只扣 1 个单位**，不是整堆；多格引用同一堆时
    // 逐格各扣 1，累计正确；该函数已被 H-21 hook，会按类别从扩展袋补扣）。
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        void* it = items[i];
        if (it == nullptr) continue;
        if (item_is_stackable(it)) {
            if (fn_inven_remove_item_data != nullptr) {
                fn_inven_remove_item_data(static_cast<int32_t>(item_category(it)), 1);
            }
        } else if (fn_remove_item != nullptr) {
            fn_remove_item(it);
        }
    }
    // 产物创建走**按类别分派的统一入口** ITEMSYSTEM_CreatePerfectItem（0x10c600）：宝石经 MakeJewel
    // 掷数值 + 写属性位、装备类掷品质；直接用 CreateItem 只会得到默认值（真机实测宝石 = 1024 = 0x400）。
    // 该符号未就绪时退回 CreateItem（fail-safe，仅数值为默认）。
    void* product = nullptr;
    if (fn_item_create_perfect_item != nullptr) {
        product = fn_item_create_perfect_item(static_cast<int32_t>(product_category));
    } else if (fn_create_item != nullptr) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "three slot craft fallback reason=no_perfect_item");
        product = fn_create_item(static_cast<int32_t>(product_category), 0, 0, 0);
    }
    if (product == nullptr) return CraftOutcome::kNoBagSpace;

    // 数值缩放写回（§2.8 混沌卷轴配方）：只改数值位（bits0-10），保留源宝石的随机等级与
    // 属性类型（bits11-23 原样搬用）；上限由 scaled_jewel_value 钳到 JEWEL_VALUE_MASK。
    if (has_source_jewel) {
        const int new_value = scaled_jewel_value(
            static_cast<int>(source_jewel_word & JEWEL_VALUE_MASK), recipe->scale_permille);
        uint32_t* word_ptr =
            reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(product) + I_JEWEL_VALUE_WORD);
        *word_ptr = (source_jewel_word & ~JEWEL_VALUE_MASK) |
                    (static_cast<uint32_t>(new_value) & JEWEL_VALUE_MASK);
        if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
            QOL_LOG_DEBUG(QolDomain::kCustomRecipe,
                          "three slot scale category=%u value=%u->%d permille=%u",
                          static_cast<uint32_t>(product_category),
                          static_cast<uint32_t>(source_jewel_word & JEWEL_VALUE_MASK), new_value,
                          static_cast<uint32_t>(recipe->scale_permille));
        }
    }
    // 孔位/强化拉满（「两件相同特殊装备 + 元气恢复药水」配方）：只写两段、其余位保持产物原值。
    //   I_SOCKET (u8)  bits4-7 = 宝石孔总数，4 位 → 最大 15；bits0-3（已镶嵌数）不写。
    //   I_ENCHANT(u16) bits2-5 = 剩余强化次数，4 位 → 最大 15；bits6-10（已强化次数）与
    //                  bits11-15（强化 ID）保持 0 不写——人工写入会造成游戏内不可能状态
    //                  （实测：已强化=2 且强化 ID=0 时 ITEMSYSTEM_EnchantItem 返回 2 并弹
    //                  「与一般的强化卷轴不同」），与 data_op_add_item 的同口径一致。
    if (recipe->product_mode == ProductMode::kMaxSocketEnchantFirstItem) {
        uint8_t* p_socket = static_cast<uint8_t*>(product) + I_SOCKET;
        *p_socket = static_cast<uint8_t>((*p_socket & 0x0Fu) | (0x0Fu << 4));
        uint16_t* p_enchant =
            reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(product) + I_ENCHANT);
        *p_enchant = static_cast<uint16_t>((*p_enchant & ~(0x0Fu << 2)) | (0x0Fu << 2));
        if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
            QOL_LOG_DEBUG(QolDomain::kCustomRecipe,
                          "three slot max socket/enchant category=%u socket=0x%02x enchant=0x%04x",
                          static_cast<uint32_t>(product_category), *p_socket, *p_enchant);
        }
    }
    if (fn_inven_save_item == nullptr) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "three slot craft abort reason=no_save_item");
        // 走到这里材料已扣、产物已建，只是入包漏斗不可用：先释放产物避免泄漏（与下面入包
        // 失败分支同口径）。
        if (fn_itempool_free != nullptr) fn_itempool_free(product);
        return CraftOutcome::kNoBagSpace;
    }
    const int saved = fn_inven_save_item(product, nullptr);
    if ((static_cast<uint32_t>(saved) & 0xffu) == 0) {
        // 原版判据（0xc0a2c uxtb / cbz → 0xc0a6c）：入包失败 → 释放产物 + 弹 5。
        if (fn_itempool_free != nullptr) fn_itempool_free(product);
        return CraftOutcome::kNoBagSpace;
    }
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "three slot crafted product=%u consumed=%d",
                      static_cast<uint32_t>(product_category),
                      (items[0] != nullptr) + (items[1] != nullptr) + (items[2] != nullptr));
    }
    return CraftOutcome::kOk;
}

int make_item(int32_t mix_type, void** out_item) {
    const Def* def = module_recipe_for_mix_type(static_cast<uint32_t>(mix_type));
    if (def == nullptr) return -1;  // 非本层配方：界面转调原版
    // fail-closed：停用期间绝不回落到原版 —— 注入记录号对原表是越界下标，
    // 原版通用路径会用它去读 RECIPEBASE 之外的字节。
    if (!enabled()) return 1;
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
    if (!attr_range_probe_jewel_range(type, item, &min, &max)) return 1;
    attr_range::Tier target = attr_range::Tier::Grey;
    int new_value = 0;
    const UpgradeResult result =
        compute_tier_up_value(value, min, max, fn_math_get_random, &target, &new_value);
    if (result != UpgradeResult::kOk) {
        return 1;  // 金档 / 退化 → 失败兜底（与合成键前置拦截互为双保险）
    }
    // read-modify-write：仅改 bits0-10，保留 bits11-17（等级）与 bits18-23（属性类型）。
    *word_ptr = (word & ~static_cast<uint32_t>(JEWEL_VALUE_MASK)) |
                (static_cast<uint32_t>(new_value) & JEWEL_VALUE_MASK);
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe,
                      "make item tier-up type=%d value=%d->%d range=[%d,%d] target=%d", type, value,
                      new_value, min, max, tier_ordinal(target));
    }
    return 0;
}

}  // namespace custom_recipe
