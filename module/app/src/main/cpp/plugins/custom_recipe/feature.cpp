// custom_recipe feature.cpp —— 自定义合成配方「结果层」（custom-craft-recipe）。
//
// 迁移自 feature/custom_recipe/custom_recipe_api.cpp（C2 Step 4）：结果层以 contracts/
// craft_result.h 的 POD 能力契约对外提供，界面状态一律入参携带（provider 不读 UIMix）；
// 扣料/产物/入包与 stuffList 需求写回属「写入执行」，同步留在本 feature。
//
// 分层（**界面归界面，结果归结果**）：
//   · 本层只回答「这 3 格是什么结果 / 该产出什么 / 该扣什么」，不认识界面。
//   · 界面层 plugins/craft_ui 经 infra/capabilities 的 `craft_result` 能力消费本层结果。

#include "plugins/custom_recipe/feature.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "contracts/attribute_range.h"
#include "core/native/qol_log.h"
#include "core/native/save_enter.h"
#include "data/native/item_class.h"
#include "plugins/custom_recipe/catalog.h"
#include "plugins/custom_recipe/rules.h"
#include "plugins/custom_recipe/table.h"
#include "game/access/craft.h"
#include "game_access.h"
#include "game_symbols.h"
#include "infra/capabilities.h"

namespace qol::features::custom_recipe {

// 纯逻辑（::custom_recipe 命名空间）在本命名空间内被同名子命名空间遮蔽，显式引入。
using ::custom_recipe::compute_tier_up_value;
using ::custom_recipe::execute_token_valid;
using ::custom_recipe::scaled_jewel_value;
using ::custom_recipe::slot_add_allowed;
using ::custom_recipe::stack_units_available;
using ::custom_recipe::tier_ordinal;
using ::custom_recipe::UpgradeResult;

namespace {

std::atomic<bool> g_custom_recipe_enabled{false};

// 真机验收用的有界诊断日志：每次进程最多 16 条，避免放料/合成高频刷屏。
std::atomic<int> g_verify_log_budget{16};

constexpr size_t kThreeSlotStuffCount = 3;

// Form → 借用的原版 type（UIMIX_SLOT_TYPE 语义：0=药水 1=宝石 2=打孔 3=混沌 4=传说）。
// 绝不 SetType(>=5)：原版 ResetActiveControl 对 type>=5 走未初始化寄存器路径。
constexpr int64_t kFormTypeMultiInputCreate = 1;  // 填入多个物品 → 生成
constexpr int64_t kFormTypeTargetAndCreate = 3;   // 填入一个物品 + 消耗材料 → 生成/原地修改
constexpr int64_t kFormTypeConsumeToCreate = 4;   // 直接消耗材料生成物品
constexpr int64_t kNoBorrowedType = -1;

// 放料 / 合成按钮 / 产物三个路径的统一介入判定：当前 mixType 是模块配方且
// kind == kJewelTierUp（与总开关无关，停用后仍需识别以 fail-closed）。
const ::custom_recipe::Def* module_recipe_for_mix_type(uint32_t mix_type) {
    const ::custom_recipe::Def* def = ::custom_recipe::def_for_mix_type(mix_type);
    if (def == nullptr || def->kind != ::custom_recipe::Kind::kJewelTierUp) return nullptr;
    return def;
}

// 物品类别 = `item + I_TYPE`(u16) 的 bits6-15；空物品返回 0 = 「该槽为空」的匹配键。
uint16_t item_category(void* item) {
    if (item == nullptr) return 0;
    const uint16_t type_flags =
        *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(item) + I_TYPE);
    return static_cast<uint16_t>((type_flags >> I_TYPE_CATEGORY_SHIFT) & I_TYPE_CATEGORY_MASK);
}

// 是否可堆叠（ITEMCLASSBASE[category*stride+6] bit0；口径与 data/native/item_class.h 一致）。
bool item_is_stackable(void* item) {
    return item_count_encoding(item) == stack_codec::CountEncoding::kEncoded;
}

// 某类别当前持有总数（INVEN_GetItemCount）；读不到返回 0（fail-closed）。
int category_held_count(uint16_t category) {
    return qol::game::craft_category_held_count(category);
}

bool item_is_jewel(void* item) {
    if (item == nullptr || fn_is_jewel == nullptr) return false;
    return fn_is_jewel(static_cast<int>(item_category(item))) != 0;
}

// 过渡期消费方式：每次调用按能力查找 attr_range 提供者，不缓存指针跨帧使用。
const qol::contracts::AttrRangeApi* attr_range_api() {
    return static_cast<const qol::contracts::AttrRangeApi*>(qol::infra::require_capability(
        qol::contracts::kAttrRangeCapabilityId,
        QolVersionRange{static_cast<uint16_t>(qol::contracts::kAttrRangeVersionMajor),
                        static_cast<uint16_t>(qol::contracts::kAttrRangeVersionMinor)}));
}

// 读宝石数值位域并按 attr_range classify 判定当前档；能力不可用/探测失败返回 false。
bool jewel_tier(void* item, attr_range::Tier* out_tier, int* out_value, int* out_min,
                int* out_max) {
    if (item == nullptr || out_tier == nullptr) return false;
    const qol::contracts::AttrRangeApi* api = attr_range_api();
    if (api == nullptr || api->probe_jewel_range == nullptr) return false;
    const uint32_t word =
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(item) + I_JEWEL_VALUE_WORD);
    const int type = static_cast<int>((word >> JEWEL_TYPE_SHIFT) & JEWEL_TYPE_MASK);
    const int value = static_cast<int>(word & JEWEL_VALUE_MASK);
    int min = 0;
    int max = 0;
    if (!api->probe_jewel_range(type, item, &min, &max)) return false;
    *out_tier = attr_range::classify(value, min, max);
    if (out_value != nullptr) *out_value = value;
    if (out_min != nullptr) *out_min = min;
    if (out_max != nullptr) *out_max = max;
    return true;
}

// Form → 借用的原版 type（0..4）。三枚举全覆盖，末行仅防御。
int64_t form_borrowed_type(::custom_recipe::Form form) {
    switch (form) {
        case ::custom_recipe::Form::kConsumeToCreate: return kFormTypeConsumeToCreate;
        case ::custom_recipe::Form::kTargetAndCreate: return kFormTypeTargetAndCreate;
        case ::custom_recipe::Form::kMultiInputCreate: return kFormTypeMultiInputCreate;
    }
    return kFormTypeTargetAndCreate;
}

// 按 Def::count_rule 重算材料需求数并写回 stuffList 条目 +6（u16）。
// 该字段是扣料（UseStuff 读 +6）与显示（UIMix_Draw 每帧读 +6）的唯一真源（§3.9）。
void apply_material_count(const ::custom_recipe::Def* def, void* jewel) {
    if (def == nullptr || jewel == nullptr) return;
    if (def->count_rule != ::custom_recipe::CountRule::kJewelGradeAndLevel) return;
    if (def->material_count == 0) return;
    const int grade = ::custom_recipe::jewel_grade_from_category(static_cast<int>(item_category(jewel)));
    if (grade < 1) return;
    const int level = qol::game::craft_character_level();
    for (uint8_t i = 0; i < def->material_count; ++i) {
        const int want =
            ::custom_recipe::material_count_for(def->materials[i].count, grade, level);
        qol::game::craft_write_stuff_need(static_cast<size_t>(i),
                                          static_cast<uint16_t>(want));
    }
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "material need grade=%d level=%d base=%d need=%d",
                      grade, level, def->materials[0].count,
                      ::custom_recipe::material_count_for(def->materials[0].count, grade, level));
    }
}

// 待合成的命中条目（YesNo 回调与放料/合成同在游戏 UI 主线程，无需加锁）。
const ::custom_recipe::ThreeSlotRecipe* g_pending_three_slot = nullptr;
// pending/token 代际：每次 prepare 命中自增并写 pending_token；停用时自增使任何在途确认失效。
uint32_t g_three_slot_token_seq = 0;
uint32_t g_pending_three_slot_token = 0;

// `kAnySpecialEquipSlot` 的判定谓词：类别是否带 ITEMCLASSBASE 记录 +7 bit4（NPC 专属保护）。
bool is_special_equip_category(uint16_t category) {
    return category_is_no_equip(static_cast<int>(category));
}

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

size_t text_length_bounded(const char* text, size_t max) {
    if (text == nullptr) return 0;
    size_t n = 0;
    while (n < max && text[n] != '\0') ++n;
    return n;
}

// ---- 3 格隐式配方工具 ----

void three_slot_refresh_pending(const ::custom_recipe::ThreeSlotRecipe* recipe) {
    g_pending_three_slot = recipe;
    g_pending_three_slot_token = recipe != nullptr ? ++g_three_slot_token_seq : 0;
}

// make_item 契约的 int64 随机源 → rules 层 int 随机源桥。
const qol::contracts::CraftMakeItemInput* g_make_item_input = nullptr;

int make_item_rand_bridge(int lo, int hi) {
    if (g_make_item_input == nullptr || g_make_item_input->rand_inclusive == nullptr) return lo;
    return static_cast<int>(g_make_item_input->rand_inclusive(lo, hi));
}

// ---- 动态特殊装备配方：进档随机重建 ----

void refresh_dynamic_recipes(void* /*ctx*/) {
    ::custom_recipe::ThreeSlotRecipe buffer[::custom_recipe::kMaxDynamicRecipes];
    const size_t n = ::custom_recipe::build_dynamic_recipes(
        ::custom_recipe::kSpecialEquipCategories, ::custom_recipe::kSpecialEquipCategoryCount,
        ::custom_recipe::kMaterialPool, ::custom_recipe::kMaterialPoolSize, fn_math_get_random, buffer,
        ::custom_recipe::kMaxDynamicRecipes);
    ::custom_recipe::set_dynamic_three_slot_recipes(buffer, n);
    QOL_LOG_INFO(QolDomain::kCustomRecipe,
                 "dynamic special recipes rebuilt count=%zu equip=%zu materialPool=%zu",
                 n, ::custom_recipe::kSpecialEquipCategoryCount, ::custom_recipe::kMaterialPoolSize);
}

// ===========================================================================
// CraftResultApi 实现（provider；界面状态一律入参）
// ===========================================================================

bool api_enabled() { return enabled(); }

bool api_three_slot_mode_active(uint32_t mix_type) {
    if (!enabled()) return false;
    const ::custom_recipe::Def* def = ::custom_recipe::def_for_mix_type(mix_type);
    return def != nullptr && def->kind == ::custom_recipe::Kind::kThreeSlotCraft;
}

bool api_is_jewel_tier_up_recipe(uint32_t mix_type) {
    return module_recipe_for_mix_type(mix_type) != nullptr;
}

int64_t api_form_ui_type_for_mix_type(uint32_t mix_type) {
    // 停用后不再映射到模块借用 type：残留面板上的配方点击不得触发 SetType 换型。
    if (!enabled()) return kNoBorrowedType;
    const ::custom_recipe::Def* def = ::custom_recipe::def_for_mix_type(mix_type);
    if (def == nullptr) return kNoBorrowedType;
    return form_borrowed_type(def->form);
}

qol::contracts::CraftGate api_craft_gate(void* target_item) {
    using qol::contracts::CraftGate;
    if (target_item == nullptr || !item_is_jewel(target_item)) return CraftGate::kNotJewel;
    attr_range::Tier tier = attr_range::Tier::Grey;
    if (jewel_tier(target_item, &tier, nullptr, nullptr, nullptr) &&
        tier == attr_range::Tier::Gold) {
        return CraftGate::kAlreadyGold;
    }
    return CraftGate::kPass;
}

bool api_is_jewel_item(void* item) { return item_is_jewel(item); }

qol::contracts::PlaceOutcome api_place(const qol::contracts::CraftPlaceInput* in) {
    using qol::contracts::PlaceOutcome;
    if (in == nullptr || !enabled()) return PlaceOutcome::kNotMine;
    void* item = in->item;
    const bool valid_slot =
        in->slot_index >= 0 && in->slot_index < static_cast<int64_t>(kThreeSlotStuffCount);
    if (item == nullptr || !valid_slot) return PlaceOutcome::kPlaced;  // 无事发生

    //   · 不可堆叠物品 = 1 个对象：同一对象不得占两格 → kAlreadyPlaced；
    //   · 可堆叠物品 = 1 个单位：同一堆可占多格，但受类别持有总数约束 → kNotEnoughHeld。
    if (in->placed_same_object > 0 && !in->item_stackable) return PlaceOutcome::kAlreadyPlaced;
    if (in->item_stackable) {
        if (!slot_add_allowed(in->same_category_placed, in->category_held_count)) {
            return PlaceOutcome::kNotEnoughHeld;
        }
    }
    void* slot = qol::game::craft_stuff_slot_control(static_cast<size_t>(in->slot_index));
    if (slot == nullptr) return PlaceOutcome::kPlaced;
    qol::game::craft_desc_set_off();  // 与原版放料前一致（0xc2364）
    qol::game::craft_set_stuff_item(static_cast<size_t>(in->slot_index), item);
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "three slot place idx=%lld category=%u",
                      static_cast<long long>(in->slot_index),
                      static_cast<uint32_t>(item_category(item)));
    }
    return PlaceOutcome::kPlaced;
}

qol::contracts::PrepareOutcome api_prepare(const qol::contracts::CraftPrepareInput* in) {
    using qol::contracts::PrepareOutcome;
    if (in == nullptr || !enabled()) return PrepareOutcome::kNotMine;
    uint16_t categories[kThreeSlotStuffCount] = {in->slot_categories[0], in->slot_categories[1],
                                                 in->slot_categories[2]};
    const ::custom_recipe::ThreeSlotRecipe* recipe =
        ::custom_recipe::match_three_slot(categories, &is_special_equip_category);
    if (recipe == nullptr) {
        if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
            QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "three slot miss slots=%u,%u,%u", categories[0],
                          categories[1], categories[2]);
        }
        return PrepareOutcome::kNoMatch;
    }
    three_slot_refresh_pending(recipe);  // 待 YesNo 确认；确认框由界面层弹
    return PrepareOutcome::kConfirmPending;
}

qol::contracts::CraftExecuteResult api_execute(const qol::contracts::CraftExecuteInput* in) {
    using ::custom_recipe::ProductMode;
    using ::custom_recipe::ThreeSlotRecipe;
    using qol::contracts::CraftExecuteResult;
    if (in == nullptr) return CraftExecuteResult::kAborted;
    const ThreeSlotRecipe* recipe = g_pending_three_slot;
    const uint32_t token = g_pending_three_slot_token;
    g_pending_three_slot = nullptr;
    g_pending_three_slot_token = 0;
    // 停用/无待确认/代际失效（残留面板上关开关后点确认）→ 不消耗、不产出、不扣费。
    if (!execute_token_valid(enabled(), token, g_three_slot_token_seq)) {
        return CraftExecuteResult::kAborted;
    }
    if (recipe == nullptr) return CraftExecuteResult::kAborted;  // 重复回调/已被消费

    // 回调前重新读格（调用方携带）：确认框停留期间用户可能改动填入格。
    void* items[kThreeSlotStuffCount] = {in->items[0], in->items[1], in->items[2]};

    // 产物 = 第 1 格物品自身时，必须在**扣料前**抓取源物品的宝石字与类别（扣料会销毁对象）。
    uint16_t product_category = recipe->product;
    uint32_t source_jewel_word = 0;
    bool has_source_jewel = false;
    if (recipe->product_mode == ProductMode::kScaleFirstItem) {
        if (items[0] == nullptr || !item_is_jewel(items[0])) {
            return CraftExecuteResult::kStaleSlots;
        }
        product_category = item_category(items[0]);
        source_jewel_word = *reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(items[0]) + I_JEWEL_VALUE_WORD);
        has_source_jewel = true;
    } else if (recipe->product_mode == ProductMode::kMaxSocketEnchantFirstItem) {
        if (items[0] == nullptr) return CraftExecuteResult::kStaleSlots;
        product_category = item_category(items[0]);
    }
    // **扣料前必须复核库存（fail-closed）**：同一堆要扣的单位数大于其当前数量 → 中止。
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        void* it = items[i];
        if (it == nullptr || !item_is_stackable(it)) continue;
        const uint16_t category = item_category(it);
        int units = 0;
        for (size_t j = 0; j < kThreeSlotStuffCount; ++j) {
            if (items[j] != nullptr && item_category(items[j]) == category) ++units;
        }
        const int held = in->held_counts[i] != 0 ? in->held_counts[i]
                                                 : category_held_count(category);
        if (!stack_units_available(units, held)) {
            return CraftExecuteResult::kNotEnoughHeld;
        }
    }
    // 扣料分两类：不可堆叠槽 → INVEN_RemoveItem；可堆叠槽 → INVEN_RemoveItemData(category,1)。
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
    // 产物创建走按类别分派的统一入口 ITEMSYSTEM_CreatePerfectItem；未就绪退回 CreateItem。
    void* product = nullptr;
    if (fn_item_create_perfect_item != nullptr) {
        product = fn_item_create_perfect_item(static_cast<int32_t>(product_category));
    } else if (fn_create_item != nullptr) {
        QOL_LOG_WARN(QolDomain::kCustomRecipe, "three slot craft fallback reason=no_perfect_item");
        product = fn_create_item(static_cast<int32_t>(product_category), 0, 0, 0);
    }
    if (product == nullptr) return CraftExecuteResult::kNoBagSpace;

    // 数值缩放写回：只改数值位，保留源宝石的随机等级与属性类型。
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
    // 孔位/强化拉满：只写两段，其余位保持产物原值。
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
        if (fn_itempool_free != nullptr) fn_itempool_free(product);
        return CraftExecuteResult::kNoBagSpace;
    }
    const int saved = fn_inven_save_item(product, nullptr);
    if ((static_cast<uint32_t>(saved) & 0xffu) == 0) {
        if (fn_itempool_free != nullptr) fn_itempool_free(product);
        return CraftExecuteResult::kNoBagSpace;
    }
    if (qol_log_debug_enabled() && g_verify_log_budget.fetch_sub(1) > 0) {
        QOL_LOG_DEBUG(QolDomain::kCustomRecipe, "three slot crafted product=%u consumed=%d",
                      static_cast<uint32_t>(product_category),
                      (items[0] != nullptr) + (items[1] != nullptr) + (items[2] != nullptr));
    }
    return CraftExecuteResult::kOk;
}

void api_material_needs(const qol::contracts::CraftMaterialNeedInput* in,
                        qol::contracts::CraftMaterialNeedOutput* out) {
    if (out != nullptr) {
        out->count = 0;
        for (int i = 0; i < 8; ++i) out->needs[i] = 0;
    }
    if (in == nullptr || out == nullptr) return;
    const ::custom_recipe::Def* def = ::custom_recipe::def_for_mix_type(in->mix_type);
    if (def == nullptr || def->count_rule != ::custom_recipe::CountRule::kJewelGradeAndLevel) return;
    if (def->material_count == 0 || in->jewel_grade < 1) return;
    int count = def->material_count;
    if (count > 8) count = 8;
    out->count = count;
    for (int i = 0; i < count; ++i) {
        out->needs[i] = ::custom_recipe::material_count_for(def->materials[i].count, in->jewel_grade,
                                                          in->character_level);
    }
}

int32_t api_make_item(const qol::contracts::CraftMakeItemInput* in) {
    if (in == nullptr) return 1;
    const ::custom_recipe::Def* def = module_recipe_for_mix_type(in->mix_type);
    if (def == nullptr) return -1;  // 非本层配方：界面转调原版
    // fail-closed：停用期间绝不回落到原版（注入记录号对原表是越界下标）。
    if (!enabled()) return 1;
    if (in->item == nullptr || in->rand_inclusive == nullptr) return 1;
    void* item = in->item;
    // 扣料数量定稿：StartMix 在 MakeItem 返回 0 之后紧接着调 UseStuff 读 stuffList。
    apply_material_count(def, item);
    uint32_t* word_ptr =
        reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(item) + I_JEWEL_VALUE_WORD);
    const uint32_t word = *word_ptr;
    const int type = static_cast<int>((word >> JEWEL_TYPE_SHIFT) & JEWEL_TYPE_MASK);
    const int value = static_cast<int>(word & JEWEL_VALUE_MASK);
    int min = 0;
    int max = 0;
    const qol::contracts::AttrRangeApi* api = attr_range_api();
    if (api == nullptr || api->probe_jewel_range == nullptr) return 1;  // 降级：不扣料不生成
    if (!api->probe_jewel_range(type, item, &min, &max)) return 1;
    attr_range::Tier target = attr_range::Tier::Grey;
    int new_value = 0;
    g_make_item_input = in;
    const UpgradeResult result =
        compute_tier_up_value(value, min, max, &make_item_rand_bridge, &target, &new_value);
    g_make_item_input = nullptr;
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

bool api_desc_should_replace(uint32_t mix_type, const char* text) {
    // 门控（按**内容**而非指针）：
    //   ① 总开关启用 ② mixType 命中模块记录 ③ 文本以原版模板前缀「用3个」开头
    //   ④ 原文本不短于模块文案（只做缩短替换，绝不放长）。
    if (!enabled()) return false;
    if (g_base == 0 || ::custom_recipe::def_for_mix_type(mix_type) == nullptr) return false;
    if (!desc_text_matches_recipe_template(text)) return false;
    return text_length_bounded(text, kDescTextScanMax) >=
           (sizeof(::custom_recipe::kModuleRecipeDesc) - 1);
}

const qol::contracts::CraftResultApi kCraftResultApi = {
    &api_enabled,
    &api_three_slot_mode_active,
    &api_is_jewel_tier_up_recipe,
    &api_form_ui_type_for_mix_type,
    &api_craft_gate,
    &api_is_jewel_item,
    &api_place,
    &api_prepare,
    &api_execute,
    &api_material_needs,
    &api_make_item,
    &api_desc_should_replace,
};

// ---- ownership ops：RECIPEBASE/MIXTUREBASE 表注入 ----

int32_t own_acquire(const char* /*id*/) {
    return ::custom_recipe::custom_recipe_table_ensure() ? 0 : 1;
}

void own_release(const char* /*id*/) {
    ::custom_recipe::custom_recipe_table_deactivate();  // total，永不失败
}

const qol::contracts::OwnershipOps kOwnershipOps = {&own_acquire, &own_release};

}  // namespace

bool enabled() { return g_custom_recipe_enabled.load(std::memory_order_acquire); }

bool set_enabled(bool enabled_value) {
    g_custom_recipe_enabled.store(enabled_value, std::memory_order_release);
    if (!enabled_value) {
        // 作废任何在途确认（残留面板上的 YesNo）：代际自增 + 清 pending。
        g_pending_three_slot = nullptr;
        g_pending_three_slot_token = 0;
        ++g_three_slot_token_seq;
    }
    return true;
}

void register_dynamic_recipes() {
    // save_enter_register 对同一 fn+ctx 幂等，重复调用不会叠加。
    save_enter_register(&refresh_dynamic_recipes, nullptr);
}

const qol::contracts::CraftResultApi& craft_result_api() { return kCraftResultApi; }

const qol::contracts::OwnershipOps& ownership_ops() { return kOwnershipOps; }

}  // namespace qol::features::custom_recipe
