#include "game_ui_custom_recipe.h"

#include "custom_recipe_catalog.h"
#include "custom_recipe_rules.h"
#include "custom_recipe_table.h"

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

// LSPosed native hook 写入的原函数指针（call_orig）。
UIMixButtonInvenItemSelectExeFn g_backup_place = nullptr;
UIMixButtonMixingExeFn g_backup_mixing = nullptr;
MakeMixFn g_backup_make_item = nullptr;
UIMixButtonMenuListExeFn g_backup_menu = nullptr;
UIMixButtonRecipeExeFn g_backup_recipe = nullptr;
XTextCtrlSetTextControlFn g_backup_set_desc_text = nullptr;
ControlItemDrawFn g_backup_control_item_draw = nullptr;
ItemDrawPortingFn g_backup_item_draw_porting = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_attempted{false};
std::atomic<bool> g_custom_recipe_enabled{false};

// 真机验收用的有界诊断日志：每次进程最多 16 条，避免放料/合成高频刷屏。
std::atomic<int> g_verify_log_budget{16};

// 原生提示文本项（§3.7 复用既有串，不新增本地化）。
constexpr uint32_t kTextNoBagSpace = 5;       // 「背包空间不足，无法进行操作。」
constexpr uint32_t kTextOnlyJewel = 97;       // 「只有宝石道具才可以。」
constexpr uint32_t kTextMaxCrafted = 105;     // 「已经达到合成最大值，无法再次进行合成。」
// 3 格隐式配方（§2.8/§4.12）复用文本：94「材料不足。无法进行合成。」/
// 99「已放置」（原版重复放置提示）/ 106「合成成功。」/ 18「是否以当前的配方来合成？」。
constexpr uint32_t kTextInsufficientMaterial = 94;
constexpr uint32_t kTextAlreadyPlaced = 99;
constexpr uint32_t kTextCraftSuccess = 106;
constexpr uint32_t kTextConfirmCraft = 0x12;
// 合成成功音效（原版 UIMix_StartMix 收尾 `mov w0,#9; bl SOUNDSYSTEM_Play`，见 0xc09bc）。
constexpr int16_t kSoundCraftSuccess = 9;
// 宝石强化页（UIMix type 1）的填入格数（[g_uimix+0xc8] 组的前 3 个子控件）。
constexpr size_t kThreeSlotStuffCount = 3;

// 主角在队伍成员数组中的下标（PARTY_GetMember 入参；0 = 主角）。
constexpr int kPartyLeaderMemberIndex = 0;

// Form → 借用的原版 type（UIMIX_SLOT_TYPE 语义：0=药水 1=宝石 2=打孔 3=混沌 4=传说）。
// 绝不 SetType(>=5)：原版 ResetActiveControl 对 type>=5 走未初始化寄存器路径。
constexpr int64_t kFormTypeMultiInputCreate = 1;  // 填入多个物品 → 生成
constexpr int64_t kFormTypeTargetAndCreate = 3;   // 填入一个物品 + 消耗材料 → 生成/原地修改
constexpr int64_t kFormTypeConsumeToCreate = 4;   // 直接消耗材料生成物品

inline bool enabled() { return g_custom_recipe_enabled.load(std::memory_order_acquire); }

// 当前 mixType（[g_uimix+0x48]）。
uint32_t current_mix_type() {
    if (g_uimix == nullptr) return 0;
    return *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_MIXTYPE);
}

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

// 当前借用的原版 type（[g_uimix+0x38] u8）；g_uimix 空返回 -1。
int64_t current_uimix_type() {
    if (g_uimix == nullptr) return -1;
    return static_cast<int64_t>(
        *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_TYPE));
}

// 读 [+0x100+type*8]：该原版 type 的已选配方下标；越界 type 返回 0（不访问内存）。
int64_t selected_recipe_at(int64_t type) {
    if (g_uimix == nullptr || type < 0 || type >= UIMIX_RECIPE_TYPE_COUNT) return 0;
    return *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) +
                                       UIMIX_SLOT_SELECTED_RECIPE_BASE +
                                       static_cast<size_t>(type) * UIMIX_SLOT_SELECTED_RECIPE_STRIDE);
}

// 写回 [+0x100+type*8]；越界 type 直接忽略（防御，正常不可达）。
void set_selected_recipe_at(int64_t type, int64_t value) {
    if (g_uimix == nullptr || type < 0 || type >= UIMIX_RECIPE_TYPE_COUNT) return;
    *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_SELECTED_RECIPE_BASE +
                                static_cast<size_t>(type) * UIMIX_SLOT_SELECTED_RECIPE_STRIDE) =
        value;
}

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

void show_text_data(uint32_t word_id) {
    if (fn_popup_create_ok_from_textdata != nullptr) {
        fn_popup_create_ok_from_textdata(word_id, 0, 0, 0);
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
    if (!enabled()) return nullptr;
    if (g_uimix == nullptr || fn_uimix_get_type == nullptr || fn_uimix_get_type() != 1) {
        return nullptr;
    }
    const custom_recipe::Def* def = custom_recipe::def_for_mix_type(current_mix_type());
    if (def == nullptr || def->kind != custom_recipe::Kind::kThreeSlotCraft) return nullptr;
    return def;
}

// `[g_uimix+0xc8]` 组第 index 个填入格的 ControlItem 控件。
void* stuff_slot_control(size_t index) {
    if (g_uimix == nullptr || fn_control_object_get_child == nullptr) return nullptr;
    void* group =
        *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_STUFF_GROUP);
    if (group == nullptr) return nullptr;
    return fn_control_object_get_child(group, static_cast<uint32_t>(index));
}

// 第 index 填入格的当前物品（空 = nullptr）。
void* stuff_slot_item(size_t index) {
    void* ctrl = stuff_slot_control(index);
    if (ctrl == nullptr || fn_control_item_get_item == nullptr) return nullptr;
    return fn_control_item_get_item(ctrl);
}

// 当前选中填入格下标（[+0x128]，i64；-1 = 未选中）。
int64_t selected_stuff_index() {
    if (g_uimix == nullptr) return -1;
    return *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(g_uimix) +
                                       UIMIX_SLOT_SELECTED_STUFF);
}

// 选中背包物指针（[+0xd8] 组 → ControlObject_GetCursor → ControlObject_GetData → *data）。
void* selected_inven_item() {
    if (g_uimix == nullptr || fn_control_object_get_cursor == nullptr ||
        fn_control_object_get_data == nullptr) {
        return nullptr;
    }
    void* group =
        *reinterpret_cast<void**>(static_cast<uint8_t*>(g_uimix) + UIMIX_SLOT_ITEM_GROUP);
    if (group == nullptr) return nullptr;
    void* cursor = fn_control_object_get_cursor(group);
    if (cursor == nullptr) return nullptr;
    void* data = fn_control_object_get_data(cursor);
    if (data == nullptr) return nullptr;
    return *reinterpret_cast<void**>(data);
}

// 读 3 格类别（空槽 = 0）到 out[3]。
void read_three_slot_categories(uint16_t out[kThreeSlotStuffCount]) {
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        out[i] = item_category(stuff_slot_item(i));
    }
}

// 原版 UIMix_StartMix 成功收尾序列（0xc0ab8-0xc09d4）：InitMixingState →
// ResetStuffItemControl → RefreshInvenItem → SOUNDSYSTEM_Play(9) → 弹 106。
void finish_three_slot_craft() {
    if (fn_uimix_init_mixing_state != nullptr) fn_uimix_init_mixing_state();
    if (fn_uimix_reset_stuff_item_control != nullptr) fn_uimix_reset_stuff_item_control();
    if (fn_ui_mix_refresh_inven_item != nullptr) fn_ui_mix_refresh_inven_item();
    if (fn_sound_system_play != nullptr) fn_sound_system_play(kSoundCraftSuccess);
    show_text_data(kTextCraftSuccess);
}

// （产物数值生成已改为走 ITEMSYSTEM_CreatePerfectItem 统一入口，见下；此处不再需要单独补写宝石位。）

// YesNo 确认回调（x3 传入，void() 形态；原版该槽位是 UIMix_StartMix@0xc0870）。
// **扣料分两类**：不可堆叠槽 → `INVEN_RemoveItem`（按对象整堆删，已 hook，扩展袋物品同样可删）；
// 可堆叠槽 → `INVEN_RemoveItemData(category, 1)`（**只扣 1 个单位**，不是整堆；多格引用同一堆时
// 逐格各扣 1，累计正确；该函数已被 H-21 hook，会按类别从扩展袋补扣）。
// 产物 = ITEMSYSTEM_CreateItem(类别,0,0,0) → INVEN_SaveItem(item,nullptr)（唯一入包漏斗，
// 已自动处理扩展袋 R-56/R-52）。入库失败判据照原版 0xc0a28-0xc0a6c：返回值低字节为 0 →
// ITEMPOOL_Free 产物 + 弹 5。
// **扣料前必须复核库存（fail-closed）**：确认框停留期间库存可能变化（丢弃/出售/另一处消耗），
// 同一堆要扣的单位数（= 引用它的格数）大于其当前数量时 → 弹 94 并中止，**不消耗任何材料、不产出**。
void three_slot_craft_callback() {
    const custom_recipe::ThreeSlotRecipe* recipe = g_pending_three_slot;
    g_pending_three_slot = nullptr;
    if (recipe == nullptr) return;  // 无待合成条目（重复回调/已被其它路径消费）：不做任何事

    // 回调前重新读格：确认框停留期间用户可能改动填入格，一律以回调时刻的格子内容为准。
    void* items[kThreeSlotStuffCount] = {nullptr, nullptr, nullptr};
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        items[i] = stuff_slot_item(i);
    }
    // 产物 = 第 1 格物品自身（ProductMode::kScaleFirstItem）时，必须在**扣料前**抓取源物品的
    // 宝石字与类别：扣料会销毁该对象（不可堆叠 → INVEN_RemoveItem 整堆删），此后指针失效。
    uint16_t product_category = recipe->product;
    uint32_t source_jewel_word = 0;
    bool has_source_jewel = false;
    if (recipe->product_mode == custom_recipe::ProductMode::kScaleFirstItem) {
        if (items[0] == nullptr || !item_is_jewel(items[0])) {
            // 确认框停留期间第 1 格被换成了非宝石 → fail-closed：不消耗、不产出。
            show_text_data(kTextInsufficientMaterial);
            return;
        }
        product_category = item_category(items[0]);
        source_jewel_word = *reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(items[0]) + I_JEWEL_VALUE_WORD);
        has_source_jewel = true;
    }
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        void* it = items[i];
        if (it == nullptr || !item_is_stackable(it)) continue;
        const uint16_t category = item_category(it);
        int units = 0;  // 同一**类别**被几格引用 ⇒ 本次要扣几个单位（扣料也是按类别总量）
        for (size_t j = 0; j < kThreeSlotStuffCount; ++j) {
            if (items[j] != nullptr && item_category(items[j]) == category) ++units;
        }
        if (!custom_recipe::stack_units_available(units, category_held_count(category))) {
            show_text_data(kTextInsufficientMaterial);
            return;  // 中止：不消耗、不产出
        }
    }
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
    if (product == nullptr) {
        show_text_data(kTextNoBagSpace);
        return;
    }
    // 数值缩放写回（§2.8 混沌卷轴配方）：只改数值位（bits0-10），保留源宝石的随机等级与
    // 属性类型（bits11-23 原样搬用）；上限由 scaled_jewel_value 钳到 JEWEL_VALUE_MASK。
    if (has_source_jewel) {
        const int new_value = custom_recipe::scaled_jewel_value(
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
    if (fn_inven_save_item == nullptr) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "three slot craft abort reason=no_save_item");
        return;
    }
    const int saved = fn_inven_save_item(product, nullptr);
    if ((static_cast<uint32_t>(saved) & 0xffu) == 0) {
        // 原版判据（0xc0a2c uxtb / cbz → 0xc0a6c）：入包失败 → 释放产物 + 弹 5。
        if (fn_itempool_free != nullptr) fn_itempool_free(product);
        show_text_data(kTextNoBagSpace);
        return;
    }
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "three slot crafted product=%u consumed=%d",
                      static_cast<uint32_t>(product_category),
                      (items[0] != nullptr) + (items[1] != nullptr) + (items[2] != nullptr));
    }
    finish_three_slot_craft();
}

// 3 格模式的放料（不转调原函数）：任意物品可填入，**一格一件**。
//   · 不可堆叠物品（如宝石）= 1 个对象：同一对象不得占两格 → 弹 99；
//   · 可堆叠物品 = 1 个单位：同一个堆**可以**占多格（重复点添加），但受库存约束
//     「已占该堆的格数 + 1 ≤ 该堆当前数量」⇒「2 个物品不能添加 3 次」，超限弹 94。
// **不改写 [+0x128]**（选中格由原版事件与 gemcraft 的进入视图逻辑维护）。
void place_three_slot_item() {
    void* item = selected_inven_item();
    if (item == nullptr) return;
    const int64_t index = selected_stuff_index();
    if (index < 0 || static_cast<size_t>(index) >= kThreeSlotStuffCount) return;
    int placed_same = 0;
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        if (static_cast<int64_t>(i) == index) continue;
        if (stuff_slot_item(i) == item) ++placed_same;
    }
    if (placed_same > 0 && !item_is_stackable(item)) {
        show_text_data(kTextAlreadyPlaced);  // 99：不可堆叠物品不能重复放置
        return;
    }
    if (item_is_stackable(item)) {
        // 可堆叠：同一个堆**可以**占多格（只要总持有量够）。上限用**游戏自带的类别持有总数**
        // INVEN_GetItemCount(category) —— 与扣料 INVEN_RemoveItemData(category,1) 口径一致；
        // 即「2 个物品不能添加 3 次」。
        const uint16_t category = item_category(item);
        int same_category_placed = 0;
        for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
            if (static_cast<int64_t>(i) == index) continue;
            void* other = stuff_slot_item(i);
            if (other != nullptr && item_category(other) == category) ++same_category_placed;
        }
        if (!custom_recipe::slot_add_allowed(same_category_placed,
                                            category_held_count(category))) {
            show_text_data(kTextInsufficientMaterial);  // 94：持有总数不足，无法再占一格
            return;
        }
    }
    void* slot = stuff_slot_control(static_cast<size_t>(index));
    if (slot == nullptr || fn_control_item_set_item == nullptr) return;
    if (fn_ui_desc_set_off != nullptr) fn_ui_desc_set_off();  // 与原版放料前一致（0xc2364）
    fn_control_item_set_item(slot, item);
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "three slot place idx=%lld category=%u",
                      static_cast<long long>(index), static_cast<uint32_t>(item_category(item)));
    }
}

// 3 格模式的合成前置匹配：命中 → 记录条目 + 弹原版 YesNo（回调 = 模块产物流程）；
// 未命中 → 弹 94 后直接返回（不进原函数、不弹确认框）。
void mix_three_slot_craft() {
    uint16_t categories[kThreeSlotStuffCount] = {0, 0, 0};
    read_three_slot_categories(categories);
    const custom_recipe::ThreeSlotRecipe* recipe = custom_recipe::match_three_slot(categories);
    if (recipe == nullptr) {
        if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
            QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "three slot miss slots=%u,%u,%u",
                          categories[0], categories[1], categories[2]);
        }
        show_text_data(kTextInsufficientMaterial);
        return;
    }
    if (fn_popup_create_yesno_from_textdata == nullptr) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "three slot confirm skipped reason=no_popup");
        return;
    }
    g_pending_three_slot = recipe;
    fn_popup_create_yesno_from_textdata(kTextConfirmCraft, 0, 0,
                                        reinterpret_cast<void*>(&three_slot_craft_callback),
                                        nullptr, nullptr);
}

// UIMix_ButtonRecipeExe hook（原版配方按钮点击，§4.11 Form 分派）。
// 原函数把「本组数组下标」写进 [+0x100+当前type*8]、把 recipeList[下标] 写进 [+0x48]，
// 并按当前 type 走原版流程。转调**后**若 [+0x48] 命中模块配方且其 Form 借用的 type ≠ 当前
// type（宝石强化页 group 3 上：「合成」= type 1 同型不动作；「宝石强化」= type 3 需切型），
// 则 SetType(该 type) → InitMixingState → ResetStuffItemControl 按形式重建合成状态。
// 无论是否同型，点击都会把自定义数组的下标留在该 type 的已选槽里 → 转调前快照、换型后恢复，
// 防污染原版该 type 的已选槽（后续切页签时按旧槽读 recipeList 会错选/越界）。同型不做事。
void custom_recipe_wrapper(void* ctrl) {
    if (g_backup_recipe == nullptr) return;
    const int64_t clicked_type = current_uimix_type();
    const int64_t prev_selected = selected_recipe_at(clicked_type);
    g_backup_recipe(ctrl);
    const custom_recipe::Def* def = custom_recipe::def_for_mix_type(current_mix_type());
    if (def == nullptr) return;  // 原版配方：保持原版结果
    const int64_t form_type = form_borrowed_type(def->form);
    if (form_type == current_uimix_type()) return;  // 同型：原版流程已按当前 type 处理
    if (fn_uimix_set_type == nullptr || fn_uimix_init_mixing_state == nullptr ||
        fn_uimix_reset_stuff_item_control == nullptr) {
        return;
    }
    fn_uimix_set_type(form_type);
    fn_uimix_init_mixing_state();
    fn_uimix_reset_stuff_item_control();
    set_selected_recipe_at(clicked_type, prev_selected);  // 恢复点击前的已选槽
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "custom recipe picked mixType=%u type=%lld",
                      current_mix_type(), static_cast<long long>(form_type));
    }
}

// 类型/菜单按钮 hook（§4.3）：点任意页签时，本函数随即执行
// SetType → MIXSYSTEM_CreateRecipeList → 建配方按钮。注入必须早于建按钮，
// 故在此处（call_orig 之前）确保注入完成，使首次点开宝石强化页即能看到注入配方。
// gemcraft wrapper 转调原函数时同样命中此处，语义一致。
void custom_menu_wrapper(void* ctrl) {
    if (enabled()) {
        custom_recipe::custom_recipe_table_ensure();
    }
    if (g_backup_menu != nullptr) {
        g_backup_menu(ctrl);
    }
}

// 放料 hook（§4.4）：先转调原函数，再纠正非法宝石 + 强制费用 0。
// 只对 kind == kJewelTierUp 介入：kNativePassThrough（「合成」入口）与原版配方一律
// 转调原函数，放料校验/费用全部交给原版 + gemcraft，否则 3:1 宝石合成流程会被破坏。
// ---- 3 格填入格：不显示整堆数量（格子语义 = 1 个单位）----
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
void* g_three_slot_ctrl[3] = {nullptr, nullptr, nullptr};

void refresh_three_slot_ctrls() {
    for (size_t i = 0; i < 3; ++i) {
        g_three_slot_ctrl[i] = stuff_slot_control(i);
    }
}

void custom_control_item_draw_wrapper(void* ctrl) {
    if (g_backup_control_item_draw == nullptr) return;
    bool is_three_slot = false;
    if (ctrl != nullptr && g_uimix != nullptr && custom_recipe_three_slot_mode_active()) {
        for (size_t i = 0; i < 3; ++i) {
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

void custom_item_draw_porting_wrapper(void* item, int32_t x, int32_t y, int32_t type,
                                      int32_t show_count) {
    if (g_backup_item_draw_porting == nullptr) return;
    if (g_drawing_three_slot_slot.load(std::memory_order_acquire)) show_count = 0;
    g_backup_item_draw_porting(item, x, y, type, show_count);
}

void custom_place_wrapper(void* ctrl) {
    if (g_backup_place == nullptr) return;
    // 3 格隐式配方（type 1「合成」入口）：任意物品可填入 ⇒ 原版三道校验不可用，整段自实现。
    if (three_slot_def_for_current() != nullptr) {
        refresh_three_slot_ctrls();  // 面板必然打开：刷新填入格控件缓存（绘制层只比较、不解引用）
        place_three_slot_item();
        return;
    }
    // def 非空即代表「当前选中的是模块注入的 kJewelTierUp 配方」，与总开关无关
    //（停用后旧面板仍可能持有它，必须能识别以 fail-closed）。
    const custom_recipe::Def* def = module_recipe_for_mix_type(current_mix_type());
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

// 合成按钮 hook（§4.5）：只对 kJewelTierUp 前置校验（非宝石弹 97 / 已金档弹 105 并拦截），
// 否则转调原函数；「合成」入口（kNativePassThrough）的 3:1 校验与确认框全部由原版 + gemcraft 处理。
void custom_mixing_wrapper(void* ctrl) {
    if (g_backup_mixing == nullptr) return;
    // 3 格隐式配方（type 1「合成」入口）：合成前先按 3 格内容查表（§4.12）。
    // 命中 → 弹原版 YesNo、回调走模块产物流程；未命中 → 弹 94 直接返回。两条路径都**不**转调
    // 原函数 ⇒ 既不进原版 StartMix、也不产生任何费用/扣料（本模式无费用）。
    if (three_slot_def_for_current() != nullptr) {
        refresh_three_slot_ctrls();  // 同上：刷新绘制层所需的填入格控件缓存
        mix_three_slot_craft();
        return;
    }
    const custom_recipe::Def* def = module_recipe_for_mix_type(current_mix_type());
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

// 产物 hook（§4.6）：只对 kJewelTierUp 读改写宝石 bits0-10；其余（含 kNativePassThrough，
// 其 mixType 实际会被 gemcraft 改写为 12..15 后再进入原函数）一律转调原函数。
int custom_make_item_wrapper(int32_t mix_type, void** out_item) {
    const custom_recipe::Def* def = module_recipe_for_mix_type(static_cast<uint32_t>(mix_type));
    if (def == nullptr) {
        return (g_backup_make_item != nullptr) ? g_backup_make_item(mix_type, out_item) : 1;
    }
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

// ---- 配方描述文案替换：钩在**文本落点**上（两个写点唯一的收敛处）----
// 为什么不在写点钩：
//  · 宝石强化页（type 1）的描述由 `UIDesc_MakeItem@0xb36a0` 写出（x0 = **物品指针**，按物品类别
//    查 itemdesc 行取 desc id；注入记录结果占位 b2-3=0 ⇒ 类别 0 ⇒ 行 0 的 desc id = 35482 =
//    模板「用3个$S%s$B合成一个$R%s$B。」⇒ 拼出「…合成金币」，真机日志实证）；但 0xb36a0
//    **已被 attribute_range 功能 hook**（game_ui_attr_range.cpp:187），`native_hook_func` 对同一
//    地址二次挂载返回 rc=-1 → 会把整条安装链拖垮（实测）。注意它 x0 是物品指针、且刷新尾的
//    SetTextControl 第 3 参是 ControlScroll 矩形宽，与 ByID 路径不同，重放极易错。
//  · `UIDesc_MakeItemByID@0xb2eec` 在该页**零调用**（有界诊断日志真机实证）。
// 描述区里 `bl 0xb181c`（X_TEXTCTRL_SetTextControl）只有两个调用点（0xb3044 / 0xb37b4）。
// 因此改为在**共用的文本落点**上拦截：命中时先把描述缓冲内容换成模块文案，再转调原函数 ——
// 宽度/滚动/点亮全部由**原调用点用自己的实参**完成，模块既不重放刷新链、也不写任何游戏文本
// 数据（MEMORYTEXT / 35482 / *BASE 表全不动）。
// 门控三重，只拦「模块配方那条模板文本」：
//   ① ctrl == 描述文本控件（0x302d98）② text == 描述缓冲首址（0x303dc0）
//   ③ 当前 [+0x48] 命中模块记录 且 缓冲以原版模板前缀「用3个」开头
// ⇒ 放入的宝石/材料（描述是宝石自身说明，不以模板开头）与其它界面一律原样转调。
// 本 hook 可能处于 UI 事件路径，不得有任何级别日志（R4；诊断走 debug + 有界预算）。

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

void custom_set_desc_text_wrapper(void* ctrl, const char* text, uint32_t width, uint32_t f1,
                                 uint32_t f2, int32_t f3) {
    if (g_backup_set_desc_text == nullptr) return;
    static_assert(sizeof(custom_recipe::kModuleRecipeDesc) <= kDescTextScanMax,
                  "module recipe desc exceeds desc text scan bound");
    // 门控（最终版，按**内容**而非指针）。真机日志定案（见设计册 §7.28）：
    //   这段描述的写入者是 `UIMix_Draw@0xc1654` 内部（ra=0xc20a4），**逐帧重建**；
    //   它用的文本控件与文本缓冲都是面板**堆对象**（ctrl=0x7a3087ebe8 / text=0x7a3087f3e0，
    //   每次打开面板都不同），**不是**静态全局 0x302d98/0x303dc0（那是另一个 UIDesc 实例，
    //   属于装备详情面板）—— 按指针比对永远不成立，这是前几轮改动全部无效的根因。
    // 条件：① 当前 [+0x48] 命中模块记录 ② 文本以原版模板前缀「用3个」开头
    //      ③ 原文本不短于模块文案（**只做缩短替换**，绝不放长，避免越界写堆缓冲）。
    const size_t original_len = text_length_bounded(text, kDescTextScanMax);
    if (g_base != 0 && custom_recipe::def_for_mix_type(current_mix_type()) != nullptr &&
        desc_text_matches_recipe_template(text) &&
        original_len >= (sizeof(custom_recipe::kModuleRecipeDesc) - 1)) {
        auto* buf = const_cast<char*>(text);
        size_t n = 0;
        for (; custom_recipe::kModuleRecipeDesc[n] != '\0' &&
               n + 1 < sizeof(custom_recipe::kModuleRecipeDesc);
             ++n) {
            buf[n] = custom_recipe::kModuleRecipeDesc[n];
        }
        buf[n] = '\0';
        if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
            QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "desc replaced mixType=%u", current_mix_type());
        }
    }
    g_backup_set_desc_text(ctrl, text, width, f1, f2, f3);
}

// ---- 配方按钮文案（「宝石升阶」）的窗口标记 ----
// 文案与 id 表统一在 module_text 的内置表（键 `recipe.jewel_tier_up`，作用域
// Scope::kUimixRecipeButton）；本 wrapper 只负责把**配方按钮的绘制窗口**标记为该作用域。
// 为什么必须限定窗口：该条目的 label wordId（35291）同时是宝石强化页的**页签名**，而页签由
// 另一个 DrawProc（UIMix_ButtonMenuListDraw）绘制 —— 不加窗口就会把页签一起改名。
UIMixButtonRecipeDrawFn g_backup_button_recipe_draw = nullptr;

void uimix_button_recipe_draw_wrapper(void* ctrl) {
    module_text::TextScopeGuard scope(module_text::Scope::kUimixRecipeButton);
    if (g_backup_button_recipe_draw != nullptr) g_backup_button_recipe_draw(ctrl);
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

// 当前所选配方是否为「3 格隐式配方」模式（供其它 feature 判断是否让路，§4.12）。
// 只按 `[g_uimix+0x48]` → 目录 kind 判定，**不含总开关**：停用后表记录数收回，
// `def_for_mix_type` 自然不再命中注入记录，因此本函数返回 false。
bool custom_recipe_three_slot_mode_active() {
    if (g_uimix == nullptr) return false;
    const custom_recipe::Def* def = custom_recipe::def_for_mix_type(current_mix_type());
    return def != nullptr && def->kind == custom_recipe::Kind::kThreeSlotCraft;
}

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
    const uintptr_t recipe_exe =
        g_base + fn_resolve("F_UIMIX_BUTTON_RECIPE_EXE_VMA", F_UIMIX_BUTTON_RECIPE_EXE_VMA);

    if (!install_one(hook, place, reinterpret_cast<void*>(&custom_place_wrapper),
                     reinterpret_cast<void**>(&g_backup_place),
                     "UIMix_ButtonInvenItemSelectExe") ||
        !install_one(hook, mixing, reinterpret_cast<void*>(&custom_mixing_wrapper),
                     reinterpret_cast<void**>(&g_backup_mixing), "UIMix_ButtonMixingExe") ||
        !install_one(hook, make_item, reinterpret_cast<void*>(&custom_make_item_wrapper),
                     reinterpret_cast<void**>(&g_backup_make_item), "MIXSYSTEM_MakeItem") ||
        !install_one(hook, menu, reinterpret_cast<void*>(&custom_menu_wrapper),
                     reinterpret_cast<void**>(&g_backup_menu), "UIMix_ButtonMenuListExe") ||
        !install_one(hook, recipe_exe, reinterpret_cast<void*>(&custom_recipe_wrapper),
                     reinterpret_cast<void**>(&g_backup_recipe), "UIMix_ButtonRecipeExe")) {
        return false;
    }

    // 描述文案替换是**装饰性**能力：单独挂载、失败只告警，**绝不拖垮核心链**。
    // 教训：`UIDesc_MakeItem@0xb36a0` 已被 attribute_range 功能 hook，对同一地址二次挂载
    // `native_hook_func` 返回 -1；早期把它串进主链导致 5 个核心 hook 全部未安装（真机实证）。
    const uintptr_t set_desc_text =
        g_base + fn_resolve("F_XTEXTCTRL_SET_TEXT_CONTROL_VMA", F_XTEXTCTRL_SET_TEXT_CONTROL_VMA);
    if (!install_one(hook, set_desc_text, reinterpret_cast<void*>(&custom_set_desc_text_wrapper),
                     reinterpret_cast<void**>(&g_backup_set_desc_text),
                     "X_TEXTCTRL_SetTextControl")) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "desc text hook skipped reason=install_failed");
    }

    // 3 格填入格「不显示整堆数量」同样是**装饰性**能力：单独挂载、失败只告警。
    const uintptr_t control_item_draw =
        g_base + fn_resolve("F_CONTROL_ITEM_DRAW_VMA", F_CONTROL_ITEM_DRAW_VMA);
    const uintptr_t item_draw_porting =
        g_base + fn_resolve("F_ITEM_DRAW_PORTING_VMA", F_ITEM_DRAW_PORTING_VMA);
    if (!install_one(hook, control_item_draw,
                     reinterpret_cast<void*>(&custom_control_item_draw_wrapper),
                     reinterpret_cast<void**>(&g_backup_control_item_draw), "ControlItem_Draw") ||
        !install_one(hook, item_draw_porting,
                     reinterpret_cast<void*>(&custom_item_draw_porting_wrapper),
                     reinterpret_cast<void**>(&g_backup_item_draw_porting), "ITEM_DrawPorting")) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe,
                     "slot count hide hook skipped reason=install_failed");
    }

    // 配方按钮文案（模块字面量「宝石升阶」）同样是**装饰性**能力：单独挂载、失败只告警。
    const uintptr_t button_recipe_draw =
        g_base + fn_resolve("F_UIMIX_BUTTON_RECIPE_DRAW_VMA", F_UIMIX_BUTTON_RECIPE_DRAW_VMA);
    if (!install_one(hook, button_recipe_draw,
                     reinterpret_cast<void*>(&uimix_button_recipe_draw_wrapper),
                     reinterpret_cast<void**>(&g_backup_button_recipe_draw),
                     "UIMix_ButtonRecipeDraw")) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "recipe label hook skipped reason=install_failed");
    }

    g_installed.store(true, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kCustomRecipe,
                 "custom recipe hooks installed core=5 desc=%d slotcount=%d label=%d",
                 g_backup_set_desc_text != nullptr ? 1 : 0,
                 g_backup_item_draw_porting != nullptr ? 1 : 0,
                 g_backup_button_recipe_draw != nullptr ? 1 : 0);
    return true;
}
