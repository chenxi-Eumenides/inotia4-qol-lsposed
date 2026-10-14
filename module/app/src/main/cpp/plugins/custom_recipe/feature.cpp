// custom_recipe feature.cpp —— 自定义合成配方「结果层」（custom-craft-recipe）。
//
// 迁移自 feature/custom_recipe/custom_recipe_api.cpp（C2 Step 4）：结果层以 contracts/
// craft_result.h 的 POD 能力契约对外提供，界面状态一律入参携带（provider 不读 UIMix）；
// 扣料/产物/入包与 stuffList 需求写回属「写入执行」，同步留在本 feature。
//
// 分层（**界面归界面，结果归结果**）：
//   · 本层只回答「这 3 格是什么结果 / 该产出什么 / 该扣什么」，不认识界面。
//   · 界面层 plugins/craft_ui 经 infra/capabilities 的 `craft_result` 能力消费本层结果。

#include "feature.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "../../contracts/attribute_quality.h"
#include "../../contracts/events.h"
#include "../../contracts/plugin.h"
#include "catalog.h"
#include "rules.h"
#include "table.h"

extern int (*fn_math_get_random)(int, int);

namespace qol::features::custom_recipe {

namespace {

constexpr uint32_t kJewelValueMask = 2047;
constexpr uint32_t kJewelTypeShift = 18;
constexpr uint32_t kJewelTypeMask = 63;

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::CraftFactsApi* g_craft_facts = nullptr;
const contracts::QolCustomRecipeWritePort* g_write_port = nullptr;
uint64_t g_save_enter_subscription = 0;
bool valid_craft_item(qol::contracts::QolItemHandle item) {
    return item.slot != 0 && item.generation != 0;
}

bool read_jewel_word(qol::contracts::QolItemHandle item, uint32_t* out_word) {
    return valid_craft_item(item) && g_write_port != nullptr &&
           g_write_port->read_jewel_word != nullptr &&
           g_write_port->read_jewel_word(item, out_word).ok == kQolOk;
}

}  // namespace

// 纯逻辑（::custom_recipe 命名空间）在本命名空间内被同名子命名空间遮蔽，显式引入。
using ::custom_recipe::compute_tier_up_value;
using ::custom_recipe::execute_token_valid;
using ::custom_recipe::scaled_jewel_value;
using ::custom_recipe::slot_add_allowed;
using ::custom_recipe::stack_units_available;
using ::custom_recipe::tier_ordinal;
using ::custom_recipe::UpgradeResult;

void set_context(const contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    ::custom_recipe::set_context(ctx.game, ctx.sdk);
}

const contracts::QolHostSdk* host_sdk() { return g_sdk; }
const contracts::QolGameHost* game_host() { return g_game; }

void set_item_facts(const contracts::ItemFactsApi* facts) { g_item_facts = facts; }

void set_craft_facts(const contracts::CraftFactsApi* facts) { g_craft_facts = facts; }

void set_write_port(const contracts::QolCustomRecipeWritePort* port) { g_write_port = port; }

void log_message(int32_t level, const char* format, ...) {
    if (g_sdk == nullptr || g_sdk->log.write == nullptr || format == nullptr) return;
    char message[contracts::kMessageCap + 1]{};
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length <= 0) return;
    const uint32_t size = static_cast<uint32_t>(length) >= contracts::kMessageCap
                              ? contracts::kMessageCap
                              : static_cast<uint32_t>(length);
    (void)g_sdk->log.write(level, 15, "plugins/custom_recipe", message, size);
}

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
uint16_t item_category(qol::contracts::QolItemHandle item) {
    int32_t category = 0;
    if (!valid_craft_item(item) || g_item_facts == nullptr ||
        g_item_facts->category == nullptr ||
        g_item_facts->category(item, &category).ok != kQolOk || category < 0) {
        return 0;
    }
    return static_cast<uint16_t>(category);
}

bool item_category_matches(qol::contracts::QolItemHandle item, uint16_t expected) {
    int32_t category = 0;
    return valid_craft_item(item) && g_item_facts != nullptr &&
           g_item_facts->category != nullptr &&
           g_item_facts->category(item, &category).ok == kQolOk && category > 0 &&
           category == expected;
}

// 是否可堆叠（ITEMCLASSBASE[category*stride+6] bit0；口径与 game/read/item_class.h 一致）。
bool item_is_stackable(qol::contracts::QolItemHandle item) {
    int32_t is_real_equip = 0;
    if (!valid_craft_item(item) || g_item_facts == nullptr ||
        g_item_facts->is_real_equip == nullptr ||
        g_item_facts->is_real_equip(item, &is_real_equip).ok != kQolOk) {
        return false;
    }
    return is_real_equip == 0;
}

// 某类别当前持有总数（INVEN_GetItemCount）；读不到返回 0（fail-closed）。
int category_held_count(uint16_t category) {
    int32_t count = 0;
    if (g_craft_facts == nullptr || g_craft_facts->read.category_held_count == nullptr ||
        g_craft_facts->read.category_held_count(category, &count).ok != kQolOk) {
        return 0;
    }
    return count;
}

bool item_is_jewel(qol::contracts::QolItemHandle item) {
    if (!valid_craft_item(item) || g_item_facts == nullptr ||
        g_item_facts->is_jewel_category == nullptr) {
        return false;
    }
    int32_t is_jewel = 0;
    return g_item_facts->is_jewel_category(item_category(item), &is_jewel).ok == kQolOk &&
           is_jewel != 0;
}

QolResult quality_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

// 每次制作同步期查找一次 v2 API；借用指针只在当前调用内使用。
QolResult require_attribute_quality_api(const contracts::AttributeQualityApi** out_api) {
    if (out_api == nullptr) {
        return quality_error(kQolErrInvalidArgument, "null attribute quality output");
    }
    *out_api = nullptr;
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) {
        return quality_error(kQolErrNotReady, "attribute quality capability unavailable");
    }
    const void* iface = nullptr;
    const QolResult result = g_sdk->capabilities.require(
        qol::contracts::kAttributeQualityCapabilityId,
        {qol::contracts::kAttributeQualityVersionMajor,
         qol::contracts::kAttributeQualityVersionMinor},
        &iface);
    if (result.ok != kQolOk) return result;
    if (iface == nullptr) {
        return quality_error(kQolErrNotReady, "attribute quality capability returned no API");
    }
    const auto* api = static_cast<const contracts::AttributeQualityApi*>(iface);
    if (!contracts::attribute_quality_api_size_valid(api)) {
        return quality_error(kQolErrUnsupported, "invalid attribute quality v2 API");
    }
    *out_api = api;
    return contracts::ok();
}

QolResult query_jewel_quality(qol::contracts::QolItemHandle item,
                              const contracts::AttributeQualityApi** out_api,
                              contracts::AttributeQualityItem* out_item) {
    if (!valid_craft_item(item) || out_api == nullptr || out_item == nullptr) {
        return quality_error(kQolErrInvalidArgument, "invalid jewel quality query input");
    }
    const QolResult required = require_attribute_quality_api(out_api);
    if (required.ok != kQolOk) return required;

    contracts::AttributeQualityItemResult result{};
    const QolResult queried = (*out_api)->query_item(item, nullptr, &result);
    if (queried.ok != kQolOk) return queried;
    if (result.complete == 0 || result.count != 1) {
        return quality_error(kQolErrNotReady, "incomplete jewel quality result");
    }
    const contracts::AttributeQualityItem& attribute = result.attrs[0];
    if (attribute.kind != contracts::AttributeKind::Jewel) {
        return quality_error(kQolErrInvalidArgument, "quality result is not a jewel");
    }
    if (attribute.status != kQolOk) {
        return quality_error(attribute.status, "jewel quality query status failed");
    }
    if (attribute.quality.level < 0 ||
        attribute.quality.level > contracts::kAttributeQualityHighestLevel) {
        return quality_error(kQolErrInternal, "invalid jewel quality level");
    }
    *out_item = attribute;
    return contracts::ok();
}

struct QualityEvaluationContext {
    const contracts::AttributeQualityApi* api;
};

QolResult evaluate_candidate_quality(int32_t value, contracts::AttributeRange range,
                                     void* context, ::custom_recipe::Tier* out_tier) {
    if (context == nullptr || out_tier == nullptr) {
        return quality_error(kQolErrInvalidArgument, "invalid candidate quality input");
    }
    const auto* evaluation = static_cast<const QualityEvaluationContext*>(context);
    if (evaluation->api == nullptr) {
        return quality_error(kQolErrNotReady, "attribute quality API expired");
    }
    contracts::AttributeQuality quality{};
    const QolResult result = evaluation->api->evaluate(value, range, nullptr, &quality);
    if (result.ok != kQolOk) return result;
    if (quality.level < 0 || quality.level > contracts::kAttributeQualityHighestLevel) {
        return quality_error(kQolErrInternal, "invalid candidate quality level");
    }
    *out_tier = static_cast<::custom_recipe::Tier>(quality.level);
    return contracts::ok();
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
void apply_material_count(const ::custom_recipe::Def* def,
                          qol::contracts::QolItemHandle jewel) {
    if (def == nullptr || !valid_craft_item(jewel)) return;
    if (def->count_rule != ::custom_recipe::CountRule::kJewelGradeAndLevel) return;
    if (def->material_count == 0) return;
    const int grade = ::custom_recipe::jewel_grade_from_category(
        static_cast<int>(item_category(jewel)));
    if (grade < 1) return;
    int32_t level = 0;
    if (g_craft_facts == nullptr || g_craft_facts->read.character_level == nullptr ||
        g_craft_facts->read.character_level(&level).ok != kQolOk) {
        return;
    }
    for (uint8_t i = 0; i < def->material_count; ++i) {
        const int want =
            ::custom_recipe::material_count_for(def->materials[i].count, grade, level);
        if (g_write_port != nullptr && g_write_port->write_stuff_need != nullptr) {
            (void)g_write_port->write_stuff_need(
                static_cast<uint32_t>(i), def->materials[i].item_id, want);
        }
    }
    if (g_verify_log_budget.fetch_sub(1) > 0) {
        log_message(0, "material need grade=%d level=%d base=%d need=%d", grade, level,
                    def->materials[0].count,
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
    for (size_t i = 0; i < ::custom_recipe::kSpecialEquipCategoryCount; ++i) {
        if (::custom_recipe::kSpecialEquipCategories[i] == category) return true;
    }
    return false;
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
    log_message(1, "dynamic special recipes rebuilt count=%zu equip=%zu materialPool=%zu", n,
                ::custom_recipe::kSpecialEquipCategoryCount,
                ::custom_recipe::kMaterialPoolSize);
}

void refresh_dynamic_recipes_event(void* /*ctx*/, const void* /*payload*/) {
    refresh_dynamic_recipes(nullptr);
}

// ===========================================================================
// CraftResultApi 实现（provider；界面状态一律入参）
// ===========================================================================

bool api_enabled() { return enabled(); }

bool api_three_slot_mode_active(uint32_t mix_type) {
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

qol::contracts::CraftGate api_craft_gate(qol::contracts::QolItemHandle target_item) {
    using qol::contracts::CraftGate;
    const qol::contracts::AttributeQualityApi* api = nullptr;
    qol::contracts::AttributeQualityItem quality{};
    if (query_jewel_quality(target_item, &api, &quality).ok != kQolOk) {
        return CraftGate::kNotJewel;
    }
    if (quality.quality.level == qol::contracts::kAttributeQualityHighestLevel) {
        return CraftGate::kAlreadyGold;
    }
    return CraftGate::kPass;
}

bool api_is_jewel_item(qol::contracts::QolItemHandle item) { return item_is_jewel(item); }

qol::contracts::PlaceOutcome api_place(const qol::contracts::CraftPlaceInput* in) {
    using qol::contracts::PlaceOutcome;
    if (in == nullptr || !enabled()) return PlaceOutcome::kNotMine;
    const qol::contracts::QolItemHandle item = in->item;
    const bool valid_slot =
        in->slot_index >= 0 && in->slot_index < static_cast<int64_t>(kThreeSlotStuffCount);
    if (!valid_craft_item(item) || !valid_slot) return PlaceOutcome::kPlaced;  // 无事发生
    if (!item_category_matches(item, in->item_category)) return PlaceOutcome::kNotMine;

    //   · 不可堆叠物品 = 1 个对象：同一对象不得占两格 → kAlreadyPlaced；
    //   · 可堆叠物品 = 1 个单位：同一堆可占多格，但受类别持有总数约束 → kNotEnoughHeld。
    if (in->placed_same_object > 0 && !in->item_stackable) return PlaceOutcome::kAlreadyPlaced;
    if (in->item_stackable) {
        if (!slot_add_allowed(in->same_category_placed, in->category_held_count)) {
            return PlaceOutcome::kNotEnoughHeld;
        }
    }
    int32_t slot_available = 0;
    if (g_craft_facts == nullptr || g_craft_facts->read_only.stuff_slot_available == nullptr ||
        g_craft_facts->read_only.stuff_slot_available(
            static_cast<uint32_t>(in->slot_index), &slot_available).ok != kQolOk ||
        slot_available == 0) return PlaceOutcome::kPlaced;
    if (g_write_port == nullptr || g_write_port->set_stuff_item == nullptr ||
        g_write_port->set_stuff_item(static_cast<uint32_t>(in->slot_index), item).ok != kQolOk) {
        return PlaceOutcome::kWriteFailed;
    }
    if (g_write_port->description_set_off != nullptr) {
        (void)g_write_port->description_set_off();
    }
    if (g_verify_log_budget.fetch_sub(1) > 0) {
        log_message(0, "three slot place idx=%lld category=%u",
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
        if (g_verify_log_budget.fetch_sub(1) > 0) {
            log_message(0, "three slot miss slots=%u,%u,%u", categories[0], categories[1],
                        categories[2]);
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

    if (::custom_recipe::match_three_slot(in->slot_categories, &is_special_equip_category) !=
        recipe) {
        return CraftExecuteResult::kStaleSlots;
    }
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        if (in->slot_categories[i] == 0) {
            if (valid_craft_item(in->items[i])) return CraftExecuteResult::kStaleSlots;
        } else if (!item_category_matches(in->items[i], in->slot_categories[i])) {
            return CraftExecuteResult::kStaleSlots;
        }
    }

    // 回调前重新读格（调用方携带）：确认框停留期间用户可能改动填入格。
    qol::contracts::QolItemHandle items[kThreeSlotStuffCount] = {
        in->items[0], in->items[1], in->items[2]};

    // 产物 = 第 1 格物品自身时，必须在**扣料前**抓取源物品的宝石字与类别（扣料会销毁对象）。
    uint16_t product_category = recipe->product;
    uint32_t source_jewel_word = 0;
    bool has_source_jewel = false;
    if (recipe->product_mode == ProductMode::kScaleFirstItem) {
        if (!valid_craft_item(items[0]) || !item_is_jewel(items[0]) ||
            !read_jewel_word(items[0], &source_jewel_word)) {
            return CraftExecuteResult::kStaleSlots;
        }
        product_category = item_category(items[0]);
        has_source_jewel = true;
    } else if (recipe->product_mode == ProductMode::kMaxSocketEnchantFirstItem) {
        if (!valid_craft_item(items[0])) return CraftExecuteResult::kStaleSlots;
        product_category = item_category(items[0]);
    }
    // **扣料前必须复核库存（fail-closed）**：同一堆要扣的单位数大于其当前数量 → 中止。
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        const auto it = items[i];
        if (!valid_craft_item(it) || !item_is_stackable(it)) continue;
        const uint16_t category = item_category(it);
        int units = 0;
        for (size_t j = 0; j < kThreeSlotStuffCount; ++j) {
            if (valid_craft_item(items[j]) && item_category(items[j]) == category) ++units;
        }
        const int held = in->held_counts[i] != 0 ? in->held_counts[i]
                                                 : category_held_count(category);
        if (!stack_units_available(units, held)) {
            return CraftExecuteResult::kNotEnoughHeld;
        }
    }
    // 扣料分两类：不可堆叠槽 → INVEN_RemoveItem；可堆叠槽 → INVEN_RemoveItemData(category,1)。
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        const auto it = items[i];
        if (!valid_craft_item(it)) continue;
        if (item_is_stackable(it)) {
            if (g_game != nullptr && g_game->inventory_write.remove_item_data != nullptr) {
                (void)g_game->inventory_write.remove_item_data(
                    static_cast<int32_t>(item_category(it)), 1);
            }
        } else if (g_game != nullptr && g_game->inventory_write.remove_item != nullptr) {
            (void)g_game->inventory_write.remove_item(it);
        }
    }
    // 产物创建走按类别分派的统一入口 ITEMSYSTEM_CreatePerfectItem；未就绪退回 CreateItem。
    qol::contracts::QolItemHandle product = qol::contracts::kInvalidItemHandle;
    if (g_write_port != nullptr && g_write_port->create_perfect_item != nullptr) {
        (void)g_write_port->create_perfect_item(static_cast<int32_t>(product_category), &product);
    } else if (g_write_port != nullptr && g_write_port->create_item != nullptr) {
        log_message(2, "three slot craft fallback reason=no_perfect_item");
        (void)g_write_port->create_item(static_cast<int32_t>(product_category), 0, 0, 0,
                                        &product);
    }
    if (!valid_craft_item(product)) return CraftExecuteResult::kNoBagSpace;

    // 数值缩放写回：只改数值位，保留源宝石的随机等级与属性类型。
    if (has_source_jewel) {
        const int new_value = scaled_jewel_value(
            static_cast<int>(source_jewel_word & kJewelValueMask), recipe->scale_permille);
        const uint32_t product_word = (source_jewel_word & ~kJewelValueMask) |
                                      (static_cast<uint32_t>(new_value) & kJewelValueMask);
        if (g_write_port == nullptr || g_write_port->write_jewel_word == nullptr ||
            g_write_port->write_jewel_word(product, product_word).ok != kQolOk) {
            if (g_write_port != nullptr && g_write_port->free_item != nullptr) {
                (void)g_write_port->free_item(product);
            }
            return CraftExecuteResult::kNoBagSpace;
        }
        if (g_verify_log_budget.fetch_sub(1) > 0) {
            log_message(0, "three slot scale category=%u value=%u->%d permille=%u",
                        static_cast<uint32_t>(product_category),
                        static_cast<uint32_t>(source_jewel_word & kJewelValueMask), new_value,
                        static_cast<uint32_t>(recipe->scale_permille));
        }
    }
    // 孔位/强化拉满：只写两段，其余位保持产物原值。
    if (recipe->product_mode == ProductMode::kMaxSocketEnchantFirstItem) {
        if (g_write_port == nullptr || g_write_port->set_max_socket_enchant == nullptr ||
            g_write_port->set_max_socket_enchant(product).ok != kQolOk) {
            if (g_write_port != nullptr && g_write_port->free_item != nullptr) {
                (void)g_write_port->free_item(product);
            }
            return CraftExecuteResult::kNoBagSpace;
        }
        if (g_verify_log_budget.fetch_sub(1) > 0) {
            log_message(0, "three slot max socket/enchant category=%u",
                        static_cast<uint32_t>(product_category));
        }
    }
    if (g_write_port == nullptr || g_write_port->save_item == nullptr) {
        log_message(2, "three slot craft abort reason=no_save_item");
        if (g_write_port != nullptr && g_write_port->free_item != nullptr) {
            (void)g_write_port->free_item(product);
        }
        return CraftExecuteResult::kNoBagSpace;
    }
    int32_t saved = 0;
    if (g_write_port->save_item(product, &saved).ok != kQolOk) {
        saved = 0;
    }
    if ((static_cast<uint32_t>(saved) & 0xffu) == 0) {
        if (g_write_port->free_item != nullptr) {
            (void)g_write_port->free_item(product);
        }
        return CraftExecuteResult::kNoBagSpace;
    }
    if (g_verify_log_budget.fetch_sub(1) > 0) {
        log_message(0, "three slot crafted product=%u consumed=%d",
                    static_cast<uint32_t>(product_category),
                    valid_craft_item(items[0]) + valid_craft_item(items[1]) +
                        valid_craft_item(items[2]));
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
    if (!valid_craft_item(in->item) || in->rand_inclusive == nullptr) return 1;

    const qol::contracts::AttributeQualityApi* api = nullptr;
    qol::contracts::AttributeQualityItem current{};
    const QolResult quality_result = query_jewel_quality(in->item, &api, &current);
    if (quality_result.ok != kQolOk) {
        log_message(2, "make item aborted: attribute quality query failed code=%d",
                    quality_result.code);
        return 1;
    }

    uint32_t word = 0;
    if (!read_jewel_word(in->item, &word)) return 1;
    const int type = static_cast<int>((word >> kJewelTypeShift) & kJewelTypeMask);
    const int value = static_cast<int>(word & kJewelValueMask);
    if (current.attribute_id != type || current.value != value) {
        log_message(2, "make item aborted: jewel quality facts do not match item word");
        return 1;
    }

    const qol::contracts::AttributeRange range = current.range;
    const auto current_tier = static_cast<::custom_recipe::Tier>(current.quality.level);
    ::custom_recipe::Tier target = ::custom_recipe::Tier::Grey;
    int32_t new_value = 0;
    UpgradeResult upgrade_result = UpgradeResult::kDegenerate;
    QualityEvaluationContext evaluation{api};
    g_make_item_input = in;
    const QolResult computed = compute_tier_up_value(
        current_tier, range, &evaluate_candidate_quality, &evaluation,
        &make_item_rand_bridge, &upgrade_result, &target, &new_value);
    g_make_item_input = nullptr;
    if (computed.ok != kQolOk) {
        log_message(2, "make item aborted: tier evaluation failed code=%d", computed.code);
        return 1;
    }
    if (upgrade_result != UpgradeResult::kOk) {
        return 1;  // 金档 / 退化 → 失败兜底（与合成键前置拦截互为双保险）
    }
    // read-modify-write：仅改 bits0-10，保留 bits11-17（等级）与 bits18-23（属性类型）。
    const uint32_t updated_word =
        (word & ~kJewelValueMask) | (static_cast<uint32_t>(new_value) & kJewelValueMask);
    if (g_write_port == nullptr || g_write_port->write_jewel_word == nullptr ||
        g_write_port->write_jewel_word(in->item, updated_word).ok != kQolOk) return 1;
    // UseStuff 在 MakeItem 返回 0 后读取 stuffList；只有宝石写入成功才提交需求数。
    apply_material_count(def, in->item);
    if (g_verify_log_budget.fetch_sub(1) > 0) {
        log_message(0, "make item tier-up type=%d value=%d->%d range=[%d,%d] target=%d", type,
                    value, new_value, range.min, range.max, tier_ordinal(target));
    }
    return 0;
}

bool api_desc_should_replace(uint32_t mix_type, const char* text) {
    // 门控（按**内容**而非指针）：
    //   ① 总开关启用 ② mixType 命中模块记录 ③ 文本以原版模板前缀「用3个」开头
    //   ④ 原文本不短于模块文案（只做缩短替换，绝不放长）。
    if (!enabled()) return false;
    if (::custom_recipe::def_for_mix_type(mix_type) == nullptr) return false;
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
    if (g_save_enter_subscription != 0 || g_sdk == nullptr ||
        g_sdk->events.subscribe == nullptr) {
        return;
    }
    (void)g_sdk->events.subscribe(contracts::kEventSaveEnter,
                                   &refresh_dynamic_recipes_event, nullptr,
                                   &g_save_enter_subscription);
}

void unregister_dynamic_recipes() {
    if (g_save_enter_subscription != 0 && g_sdk != nullptr &&
        g_sdk->events.unsubscribe != nullptr) {
        (void)g_sdk->events.unsubscribe(g_save_enter_subscription);
    }
    g_save_enter_subscription = 0;
}

const qol::contracts::CraftResultApi& craft_result_api() { return kCraftResultApi; }

const qol::contracts::OwnershipOps& ownership_ops() { return kOwnershipOps; }

bool handle_write_stuff_need_command(const uint8_t* payload, uint32_t size) {
    if (payload == nullptr || size != sizeof(contracts::CraftWriteStuffNeedPayload) ||
        g_write_port == nullptr || g_write_port->write_stuff_need == nullptr) {
        return false;
    }
    contracts::CraftWriteStuffNeedPayload command{};
    std::memcpy(&command, payload, sizeof(command));
    return g_write_port->write_stuff_need(command.index, command.category, command.count).ok ==
           kQolOk;
}

bool handle_set_stuff_item_command(const uint8_t* payload, uint32_t size) {
    if (payload == nullptr || size != sizeof(contracts::CraftSetStuffItemPayload) ||
        g_write_port == nullptr || g_write_port->set_stuff_item == nullptr) {
        return false;
    }
    contracts::CraftSetStuffItemPayload command{};
    std::memcpy(&command, payload, sizeof(command));
    return g_write_port->set_stuff_item(command.index, command.item).ok == kQolOk;
}

}  // namespace qol::features::custom_recipe
