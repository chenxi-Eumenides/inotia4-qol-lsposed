// custom_recipe feature.cpp —— 自定义合成配方「结果层」（custom-craft-recipe）。
//
// 结果层经 result.h 的 POD 内部契约向同插件 UI 子层提供，界面状态一律入参携带（不读 UIMix）；
// 扣料/产物/入包与 stuffList 需求写回属「写入执行」，同步留在本 feature。
//
// 分层（**界面归界面，结果归结果**）：
//   · 本层只回答「这 3 格是什么结果 / 该产出什么 / 该扣什么」，不认识界面。
//   · 界面子层（plugins/custom_recipe/ui）直接调用本层内部 API，不再经 capability registry 消费。

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

QolResult recipe_error(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

bool custom_recipe_write_port_v12_ready() {
    return g_write_port != nullptr && contracts::satisfies(
        g_write_port->abi_version,
        {contracts::kQolPortCustomRecipeWriteVersion.major,
         contracts::kQolPortCustomRecipeWriteVersion.minor});
}

QolResult port_random_inclusive(int32_t lo, int32_t hi, int32_t* out) {
    if (out == nullptr || lo > hi) {
        return recipe_error(kQolErrInvalidArgument, "invalid random interval");
    }
    if (!custom_recipe_write_port_v12_ready() || g_write_port->random_inclusive == nullptr) {
        return recipe_error(kQolErrNotReady, "custom recipe random port unavailable");
    }
    const QolResult result = g_write_port->random_inclusive(lo, hi, out);
    if (result.ok != kQolOk) return result;
    if (*out < lo || *out > hi) {
        return recipe_error(kQolErrInternal, "random port returned value outside interval");
    }
    return contracts::ok();
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
bool g_ownership_ready = false;

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

bool item_is_jewel(qol::contracts::QolItemHandle item) {
    if (!valid_craft_item(item) || g_item_facts == nullptr ||
        g_item_facts->is_jewel_category == nullptr) {
        return false;
    }
    int32_t is_jewel = 0;
    return g_item_facts->is_jewel_category(item_category(item), &is_jewel).ok == kQolOk &&
           is_jewel != 0;
}

QolResult query_jewel_fact(qol::contracts::QolItemHandle item, uint16_t* out_category,
                           bool* out_is_jewel) {
    if (!valid_craft_item(item) || out_category == nullptr || out_is_jewel == nullptr ||
        g_item_facts == nullptr || g_item_facts->category == nullptr ||
        g_item_facts->is_jewel_category == nullptr) {
        return recipe_error(kQolErrNotReady, "jewel item facts unavailable");
    }
    int32_t category = 0;
    QolResult result = g_item_facts->category(item, &category);
    if (result.ok != kQolOk) return result;
    if (category <= 0 || category > UINT16_MAX) {
        return recipe_error(kQolErrInternal, "invalid item category");
    }
    int32_t is_jewel = 0;
    result = g_item_facts->is_jewel_category(category, &is_jewel);
    if (result.ok != kQolOk) return result;
    if (is_jewel != 0 && is_jewel != 1) {
        return recipe_error(kQolErrInternal, "invalid jewel category result");
    }
    *out_category = static_cast<uint16_t>(category);
    *out_is_jewel = is_jewel != 0;
    return contracts::ok();
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

struct MaterialNeedPlan {
    uint8_t count = 0;
    int32_t needs[8]{};
    int grade = 0;
    int32_t level = 0;
};

// 先完整验证需求，再由调用方依序写入 stuffList。
QolResult prepare_material_needs(const ::custom_recipe::Def* def,
                                 qol::contracts::QolItemHandle jewel,
                                 MaterialNeedPlan* out_plan) {
    if (def == nullptr || !valid_craft_item(jewel) || out_plan == nullptr ||
        def->count_rule != ::custom_recipe::CountRule::kJewelGradeAndLevel ||
        def->materials == nullptr || def->material_count == 0 || def->material_count > 8) {
        return recipe_error(kQolErrInvalidArgument, "invalid tier-up material definition");
    }
    uint16_t category = 0;
    bool is_jewel = false;
    QolResult result = query_jewel_fact(jewel, &category, &is_jewel);
    if (result.ok != kQolOk) return result;
    if (!is_jewel) return recipe_error(kQolErrInvalidArgument, "tier-up target is not a jewel");
    const int grade = ::custom_recipe::jewel_grade_from_category(static_cast<int>(category));
    if (grade < 1 || grade > 5) {
        return recipe_error(kQolErrInternal, "invalid jewel material grade");
    }
    if (g_craft_facts == nullptr || g_craft_facts->read.character_level == nullptr) {
        return recipe_error(kQolErrNotReady, "character level facts unavailable");
    }
    int32_t level = 0;
    result = g_craft_facts->read.character_level(&level);
    if (result.ok != kQolOk) return result;
    if (g_write_port == nullptr || g_write_port->write_stuff_need == nullptr) {
        return recipe_error(kQolErrNotReady, "stuff need writer unavailable");
    }

    MaterialNeedPlan plan{};
    plan.count = def->material_count;
    plan.grade = grade;
    plan.level = level;
    for (uint8_t i = 0; i < def->material_count; ++i) {
        if (def->materials[i].item_id == 0 || def->materials[i].count == 0) {
            return recipe_error(kQolErrInternal, "invalid tier-up material entry");
        }
        plan.needs[i] = ::custom_recipe::material_count_for(
            def->materials[i].count, grade, level);
        if (plan.needs[i] < 0) {
            return recipe_error(kQolErrInternal, "invalid material need");
        }
    }
    *out_plan = plan;
    return contracts::ok();
}

QolResult write_material_needs(const ::custom_recipe::Def* def,
                               const MaterialNeedPlan& plan) {
    for (uint8_t i = 0; i < plan.count; ++i) {
        const QolResult result = g_write_port->write_stuff_need(
            i, def->materials[i].item_id, plan.needs[i]);
        if (result.ok != kQolOk) {
            log_message(2, "tier-up failed stage=material_need index=%u result=%d",
                        static_cast<uint32_t>(i), result.code);
            return result;
        }
    }
    if (g_verify_log_budget.fetch_sub(1) > 0) {
        log_message(0, "material needs grade=%d level=%d count=%u first=%d", plan.grade,
                    plan.level, static_cast<uint32_t>(plan.count), plan.needs[0]);
    }
    return contracts::ok();
}

// 待确认配方、配方代际与三个 typed item identity；只在游戏主线程访问。
::custom_recipe::ThreeSlotRecipe g_pending_three_slot{};
qol::contracts::QolItemHandle g_pending_three_slot_items[kThreeSlotStuffCount]{};
uint16_t g_pending_three_slot_categories[kThreeSlotStuffCount]{};
uint64_t g_pending_recipe_generation = 0;
bool g_pending_three_slot_valid = false;
// pending/token 代际：prepare、SaveEnter 重建与 disable 都使旧确认失效。
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

void three_slot_invalidate_pending() {
    g_pending_three_slot_valid = false;
    g_pending_three_slot_token = 0;
    g_pending_recipe_generation = 0;
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        g_pending_three_slot_items[i] = qol::contracts::kInvalidItemHandle;
        g_pending_three_slot_categories[i] = 0;
    }
    ++g_three_slot_token_seq;
}

void three_slot_refresh_pending(const ::custom_recipe::ThreeSlotRecipe& recipe,
                                const qol::contracts::QolItemHandle items[kThreeSlotStuffCount],
                                const uint16_t categories[kThreeSlotStuffCount]) {
    g_pending_three_slot = recipe;
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        g_pending_three_slot_items[i] = items[i];
        g_pending_three_slot_categories[i] = categories[i];
    }
    g_pending_recipe_generation = ::custom_recipe::dynamic_recipe_generation();
    g_pending_three_slot_valid = true;
    g_pending_three_slot_token = ++g_three_slot_token_seq;
}

// make_item 的 typed 闭区间随机源 → rules 层随机源，不替错误伪造下界。
const result::CraftMakeItemInput* g_make_item_input = nullptr;

QolResult make_item_rand_bridge(int32_t lo, int32_t hi, int32_t* out) {
    if (out == nullptr || lo > hi || g_make_item_input == nullptr ||
        g_make_item_input->rand_inclusive == nullptr) {
        return recipe_error(kQolErrNotReady, "tier-up random source unavailable");
    }
    const QolResult result = g_make_item_input->rand_inclusive(lo, hi, out);
    if (result.ok != kQolOk) return result;
    if (*out < lo || *out > hi) {
        return recipe_error(kQolErrInternal, "tier-up random value outside interval");
    }
    return contracts::ok();
}

// ---- 动态特殊装备配方：进档随机重建 ----

void refresh_dynamic_recipes(void* /*ctx*/) {
    three_slot_invalidate_pending();
    ::custom_recipe::ThreeSlotRecipe buffer[::custom_recipe::kMaxDynamicRecipes];
    size_t n = 0;
    const QolResult result = ::custom_recipe::build_dynamic_recipes(
        ::custom_recipe::kSpecialEquipCategories, ::custom_recipe::kSpecialEquipCategoryCount,
        ::custom_recipe::kMaterialPool, ::custom_recipe::kMaterialPoolSize,
        &qol::features::custom_recipe::random_inclusive, buffer,
        ::custom_recipe::kMaxDynamicRecipes, &n);
    if (result.ok != kQolOk) {
        log_message(2, "dynamic recipe rebuild failed stage=random result=%d", result.code);
        return;
    }
    ::custom_recipe::set_dynamic_three_slot_recipes(buffer, n);
    log_message(1, "dynamic special recipes rebuilt count=%zu equip=%zu materialPool=%zu", n,
                ::custom_recipe::kSpecialEquipCategoryCount,
                ::custom_recipe::kMaterialPoolSize);
}

void refresh_dynamic_recipes_event(void* /*ctx*/, const void* /*payload*/) {
    refresh_dynamic_recipes(nullptr);
}

bool same_item_handle(qol::contracts::QolItemHandle left,
                      qol::contracts::QolItemHandle right) {
    return left.slot == right.slot && left.generation == right.generation;
}

bool same_three_slot_recipe(const ::custom_recipe::ThreeSlotRecipe& left,
                            const ::custom_recipe::ThreeSlotRecipe& right) {
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        if (left.slots[i] != right.slots[i]) return false;
    }
    return left.ordered == right.ordered && left.product == right.product &&
           left.product_mode == right.product_mode &&
           left.scale_permille == right.scale_permille &&
           left.same_first_last == right.same_first_last;
}

QolResult read_live_item_category(qol::contracts::QolItemHandle item, uint16_t* out_category) {
    if (!valid_craft_item(item) || out_category == nullptr || g_item_facts == nullptr ||
        g_item_facts->category == nullptr) {
        return recipe_error(kQolErrNotReady, "item category facts unavailable");
    }
    int32_t category = 0;
    const QolResult result = g_item_facts->category(item, &category);
    if (result.ok != kQolOk) return result;
    if (category <= 0 || category > UINT16_MAX) {
        return recipe_error(kQolErrInternal, "invalid item category fact");
    }
    *out_category = static_cast<uint16_t>(category);
    return contracts::ok();
}

QolResult read_item_stackability(qol::contracts::QolItemHandle item, bool* out_stackable) {
    if (!valid_craft_item(item) || out_stackable == nullptr || g_item_facts == nullptr ||
        g_item_facts->is_real_equip == nullptr) {
        return recipe_error(kQolErrNotReady, "item stackability facts unavailable");
    }
    int32_t is_real_equip = 0;
    const QolResult result = g_item_facts->is_real_equip(item, &is_real_equip);
    if (result.ok != kQolOk) return result;
    if (is_real_equip != 0 && is_real_equip != 1) {
        return recipe_error(kQolErrInternal, "invalid real equipment fact");
    }
    *out_stackable = is_real_equip == 0;
    return contracts::ok();
}

QolResult read_live_category_count(uint16_t category, int32_t* out_count) {
    if (category == 0 || out_count == nullptr || g_craft_facts == nullptr ||
        g_craft_facts->read.category_held_count == nullptr) {
        return recipe_error(kQolErrNotReady, "live category count unavailable");
    }
    const QolResult result = g_craft_facts->read.category_held_count(category, out_count);
    if (result.ok != kQolOk) return result;
    if (*out_count < 0) return recipe_error(kQolErrInternal, "negative category count");
    return contracts::ok();
}

void log_execute_stage(int32_t level, const char* stage, uint32_t index,
                       int32_t result, int32_t before, int32_t after) {
    if (level == 0 && g_verify_log_budget.fetch_sub(1) <= 0) return;
    log_message(level,
                "three slot execute stage=%s index=%u result=%d before=%d after=%d",
                stage, index, result, before, after);
}

void discard_uncommitted_product(qol::contracts::QolItemHandle product,
                                 const char* failed_stage, uint32_t index) {
    if (!valid_craft_item(product)) return;
    if (g_write_port == nullptr || g_write_port->free_item == nullptr) {
        log_message(2, "three slot execute stage=product_cleanup index=%u result=%d "
                       "after=%s ownership=unknown",
                    index, kQolErrNotReady, failed_stage);
        return;
    }
    const QolResult freed = g_write_port->free_item(product);
    if (freed.ok != kQolOk) {
        log_message(2, "three slot execute stage=product_cleanup index=%u result=%d "
                       "after=%s ownership=unknown",
                    index, freed.code, failed_stage);
    } else if (g_verify_log_budget.fetch_sub(1) > 0) {
        log_message(0, "three slot execute stage=product_cleanup index=%u result=0 "
                       "after=%s ownership=released",
                    index, failed_stage);
    }
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

result::CraftGate api_craft_gate(qol::contracts::QolItemHandle target_item) {
    using result::CraftGate;
    uint16_t category = 0;
    bool is_jewel = false;
    if (query_jewel_fact(target_item, &category, &is_jewel).ok != kQolOk) {
        return CraftGate::kUnavailable;
    }
    (void)category;
    if (!is_jewel) return CraftGate::kNotJewel;
    const qol::contracts::AttributeQualityApi* api = nullptr;
    qol::contracts::AttributeQualityItem quality{};
    if (query_jewel_quality(target_item, &api, &quality).ok != kQolOk) {
        return CraftGate::kUnavailable;
    }
    uint32_t word = 0;
    if (!read_jewel_word(target_item, &word) ||
        quality.attribute_id != static_cast<int32_t>((word >> kJewelTypeShift) & kJewelTypeMask) ||
        quality.value != static_cast<int32_t>(word & kJewelValueMask)) {
        return CraftGate::kUnavailable;
    }
    if (quality.quality.level == qol::contracts::kAttributeQualityHighestLevel) {
        return CraftGate::kAlreadyGold;
    }
    return CraftGate::kPass;
}

bool api_is_jewel_item(qol::contracts::QolItemHandle item) { return item_is_jewel(item); }

result::PlaceOutcome api_place(const result::CraftPlaceInput* in) {
    using result::PlaceOutcome;
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

result::PrepareOutcome api_prepare(const result::CraftPrepareInput* in) {
    using result::PrepareOutcome;
    if (in == nullptr || !enabled()) return PrepareOutcome::kNotMine;
    three_slot_invalidate_pending();
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
    if (g_craft_facts == nullptr || g_craft_facts->read.stuff_item == nullptr) {
        log_message(2, "three slot prepare failed stage=identity result=%d", kQolErrNotReady);
        return PrepareOutcome::kUnavailable;
    }
    qol::contracts::QolItemHandle items[kThreeSlotStuffCount] = {
        qol::contracts::kInvalidItemHandle,
        qol::contracts::kInvalidItemHandle,
        qol::contracts::kInvalidItemHandle,
    };
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        const QolResult read = g_craft_facts->read.stuff_item(static_cast<uint32_t>(i), &items[i]);
        if (read.ok != kQolOk) {
            log_message(2, "three slot prepare failed stage=identity index=%u result=%d",
                        static_cast<uint32_t>(i), read.code);
            return PrepareOutcome::kUnavailable;
        }
        if (categories[i] == 0) {
            if (items[i].slot != 0 || items[i].generation != 0) {
                // 调用方声称该格为空但槽内有对象：上下文与配方判定不一致。
                log_message(2, "three slot prepare failed stage=empty_identity index=%u result=%d",
                            static_cast<uint32_t>(i), kQolErrConflict);
                return PrepareOutcome::kUnavailable;
            }
            continue;
        }
        uint16_t actual_category = 0;
        const QolResult category_result = read_live_item_category(items[i], &actual_category);
        if (category_result.ok != kQolOk) {
            log_message(2, "three slot prepare failed stage=identity_category index=%u result=%d",
                        static_cast<uint32_t>(i), category_result.code);
            return PrepareOutcome::kUnavailable;
        }
        if (actual_category != categories[i]) {
            // 传入类别与槽内实际类别不一致：上下文错误，不是「配方不匹配」。
            log_message(2, "three slot prepare failed stage=identity_category index=%u result=%d",
                        static_cast<uint32_t>(i), kQolErrConflict);
            return PrepareOutcome::kUnavailable;
        }
    }
    three_slot_refresh_pending(*recipe, items, categories);  // YesNo 前保存完整 recipe 与 identity。
    return PrepareOutcome::kConfirmPending;
}

result::CraftExecuteResult api_execute(const result::CraftExecuteInput* in) {
    using ::custom_recipe::ProductMode;
    using ::custom_recipe::ThreeSlotRecipe;
    using result::CraftExecuteResult;
    if (in == nullptr) return CraftExecuteResult::kAborted;

    const ThreeSlotRecipe recipe = g_pending_three_slot;
    qol::contracts::QolItemHandle pending_items[kThreeSlotStuffCount] = {
        g_pending_three_slot_items[0], g_pending_three_slot_items[1],
        g_pending_three_slot_items[2]};
    uint16_t pending_categories[kThreeSlotStuffCount] = {
        g_pending_three_slot_categories[0], g_pending_three_slot_categories[1],
        g_pending_three_slot_categories[2]};
    const uint64_t pending_generation = g_pending_recipe_generation;
    const uint32_t token = g_pending_three_slot_token;
    const bool pending = g_pending_three_slot_valid;
    g_pending_three_slot_valid = false;
    g_pending_three_slot_token = 0;
    if (!pending || !execute_token_valid(enabled(), token, g_three_slot_token_seq)) {
        return CraftExecuteResult::kAborted;
    }
    if (pending_generation != ::custom_recipe::dynamic_recipe_generation()) {
        log_execute_stage(1, "precheck_generation", 0, kQolErrConflict, 0, 0);
        return CraftExecuteResult::kStaleSlots;
    }

    const ThreeSlotRecipe* matched_recipe =
        ::custom_recipe::match_three_slot(in->slot_categories, &is_special_equip_category);
    if (matched_recipe == nullptr || !same_three_slot_recipe(*matched_recipe, recipe)) {
        log_execute_stage(1, "precheck_recipe", 0, kQolErrConflict, 0, 0);
        return CraftExecuteResult::kStaleSlots;
    }

    struct CategoryPlan {
        uint16_t category = 0;
        int32_t initial_count = 0;
        int32_t current_count = 0;
        int32_t units = 0;
        bool stackable = false;
    } category_plans[kThreeSlotStuffCount]{};
    uint8_t category_for_slot[kThreeSlotStuffCount]{};
    bool stackable[kThreeSlotStuffCount]{};
    uint16_t categories[kThreeSlotStuffCount]{};
    size_t category_count = 0;
    qol::contracts::QolItemHandle items[kThreeSlotStuffCount] = {
        qol::contracts::kInvalidItemHandle, qol::contracts::kInvalidItemHandle,
        qol::contracts::kInvalidItemHandle};

    if (g_craft_facts == nullptr || g_craft_facts->read.stuff_item == nullptr) {
        log_execute_stage(2, "precheck_ready", 0, kQolErrNotReady, 0, 0);
        return CraftExecuteResult::kPrecheckFailed;
    }
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        if (in->slot_categories[i] != pending_categories[i] ||
            !same_item_handle(in->items[i], pending_items[i])) {
            log_execute_stage(1, "precheck_identity", static_cast<uint32_t>(i),
                              kQolErrConflict, 0, 0);
            return CraftExecuteResult::kStaleSlots;
        }
        const QolResult live_item =
            g_craft_facts->read.stuff_item(static_cast<uint32_t>(i), &items[i]);
        if (live_item.ok != kQolOk) {
            log_execute_stage(2, "precheck_live_identity", static_cast<uint32_t>(i),
                              live_item.code, 0, 0);
            return CraftExecuteResult::kPrecheckFailed;
        }
        if (!same_item_handle(items[i], in->items[i])) {
            log_execute_stage(1, "precheck_live_identity", static_cast<uint32_t>(i),
                              kQolErrConflict, 0, 0);
            return CraftExecuteResult::kStaleSlots;
        }
        if (in->slot_categories[i] == 0) {
            if (items[i].slot != 0 || items[i].generation != 0) {
                log_execute_stage(1, "precheck_empty_identity", static_cast<uint32_t>(i),
                                  kQolErrConflict, 0, 0);
                return CraftExecuteResult::kStaleSlots;
            }
            continue;
        }
        if (!valid_craft_item(items[i])) {
            log_execute_stage(1, "precheck_generation", static_cast<uint32_t>(i),
                              kQolErrNotFound, 0, 0);
            return CraftExecuteResult::kStaleSlots;
        }
        const QolResult category_result = read_live_item_category(items[i], &categories[i]);
        if (category_result.ok != kQolOk) {
            log_execute_stage(2, "precheck_category", static_cast<uint32_t>(i),
                              category_result.code, 0, 0);
            return category_result.code == kQolErrNotFound
                       ? CraftExecuteResult::kStaleSlots
                       : CraftExecuteResult::kPrecheckFailed;
        }
        if (categories[i] != in->slot_categories[i]) {
            log_execute_stage(1, "precheck_category", static_cast<uint32_t>(i),
                              kQolErrConflict, in->slot_categories[i], categories[i]);
            return CraftExecuteResult::kStaleSlots;
        }
        const QolResult stack_result = read_item_stackability(items[i], &stackable[i]);
        if (stack_result.ok != kQolOk) {
            log_execute_stage(2, "precheck_stackability", static_cast<uint32_t>(i),
                              stack_result.code, 0, 0);
            return CraftExecuteResult::kPrecheckFailed;
        }

        size_t category_index = 0;
        while (category_index < category_count &&
               category_plans[category_index].category != categories[i]) {
            ++category_index;
        }
        if (category_index == category_count) {
            CategoryPlan& category_plan = category_plans[category_count];
            category_plan.category = categories[i];
            const QolResult count_result =
                read_live_category_count(categories[i], &category_plan.initial_count);
            if (count_result.ok != kQolOk) {
                log_execute_stage(2, "precheck_count", static_cast<uint32_t>(i),
                                  count_result.code, 0, 0);
                return CraftExecuteResult::kPrecheckFailed;
            }
            category_plan.current_count = category_plan.initial_count;
            category_plan.stackable = stackable[i];
            ++category_count;
        } else if (category_plans[category_index].stackable != stackable[i]) {
            log_execute_stage(2, "precheck_stackability", static_cast<uint32_t>(i),
                              kQolErrConflict, 0, 0);
            return CraftExecuteResult::kPrecheckFailed;
        }
        category_for_slot[i] = static_cast<uint8_t>(category_index);
        ++category_plans[category_index].units;
        if (stackable[i]) {
            if (g_game == nullptr || g_game->inventory_write.remove_item_data == nullptr) {
                log_execute_stage(2, "precheck_remove_ready", static_cast<uint32_t>(i),
                                  kQolErrNotReady, 0, 0);
                return CraftExecuteResult::kPrecheckFailed;
            }
        } else {
            if (g_game == nullptr || g_game->inventory_write.remove_item == nullptr) {
                log_execute_stage(2, "precheck_remove_ready", static_cast<uint32_t>(i),
                                  kQolErrNotReady, 0, 0);
                return CraftExecuteResult::kPrecheckFailed;
            }
            for (size_t j = 0; j < i; ++j) {
                if (valid_craft_item(items[j]) && same_item_handle(items[j], items[i])) {
                    log_execute_stage(1, "precheck_duplicate_identity", static_cast<uint32_t>(i),
                                      kQolErrConflict, 0, 0);
                    return CraftExecuteResult::kStaleSlots;
                }
            }
        }
    }

    for (size_t i = 0; i < category_count; ++i) {
        const CategoryPlan& category_plan = category_plans[i];
        if (category_plan.initial_count < category_plan.units) {
            log_execute_stage(1, "precheck_count", static_cast<uint32_t>(i),
                              kQolErrNotFound, category_plan.initial_count,
                              category_plan.initial_count);
            return CraftExecuteResult::kNotEnoughHeld;
        }
    }

    uint16_t product_category = recipe.product;
    uint32_t source_jewel_word = 0;
    if (recipe.product_mode == ProductMode::kFixedCategory) {
        if (product_category == 0) {
            log_execute_stage(2, "precheck_product", 0, kQolErrInvalidArgument, 0, 0);
            return CraftExecuteResult::kPrecheckFailed;
        }
    } else if (recipe.product_mode == ProductMode::kScaleFirstItem) {
        uint16_t jewel_category = 0;
        bool is_jewel = false;
        const QolResult jewel_fact = query_jewel_fact(items[0], &jewel_category, &is_jewel);
        if (jewel_fact.ok != kQolOk || !is_jewel || jewel_category != categories[0] ||
            !read_jewel_word(items[0], &source_jewel_word)) {
            log_execute_stage(2, "precheck_source_jewel", 0,
                              jewel_fact.ok == kQolOk ? kQolErrNotReady : jewel_fact.code,
                              0, 0);
            return CraftExecuteResult::kPrecheckFailed;
        }
        product_category = categories[0];
    } else if (recipe.product_mode == ProductMode::kMaxSocketEnchantFirstItem) {
        if (!valid_craft_item(items[0]) || !valid_craft_item(items[2]) ||
            same_item_handle(items[0], items[2]) || categories[0] != categories[2]) {
            log_execute_stage(1, "precheck_product_identity", 0, kQolErrConflict, 0, 0);
            return CraftExecuteResult::kStaleSlots;
        }
        product_category = categories[0];
    } else {
        log_execute_stage(2, "precheck_product_mode", 0, kQolErrUnsupported, 0, 0);
        return CraftExecuteResult::kPrecheckFailed;
    }

    if ((g_write_port == nullptr ||
         (g_write_port->create_perfect_item == nullptr && g_write_port->create_item == nullptr) ||
         g_write_port->save_item == nullptr) ||
        (recipe.product_mode == ProductMode::kScaleFirstItem &&
         g_write_port->write_jewel_word == nullptr) ||
        (recipe.product_mode == ProductMode::kMaxSocketEnchantFirstItem &&
         g_write_port->set_max_socket_enchant == nullptr)) {
        log_execute_stage(2, "precheck_product_ready", 0, kQolErrNotReady, 0, 0);
        return CraftExecuteResult::kPrecheckFailed;
    }

    log_execute_stage(0, "precheck", 0, kQolOk, static_cast<int32_t>(category_count),
                      static_cast<int32_t>(kThreeSlotStuffCount));

    // 先准备并配置尚未保存的产物；此时输入 handle 尚未被扣料销毁。
    qol::contracts::QolItemHandle product = qol::contracts::kInvalidItemHandle;
    QolResult product_result = contracts::ok();
    if (g_write_port->create_perfect_item != nullptr) {
        product_result = g_write_port->create_perfect_item(
            static_cast<int32_t>(product_category), &product);
    } else {
        product_result = g_write_port->create_item(
            static_cast<int32_t>(product_category), 0, 0, 0, &product);
    }
    log_execute_stage(product_result.ok == kQolOk && valid_craft_item(product) ? 0 : 2,
                      "product_create", 0,
                      product_result.ok == kQolOk && valid_craft_item(product)
                          ? kQolOk
                          : (product_result.ok == kQolOk ? kQolErrInternal : product_result.code),
                      0, valid_craft_item(product) ? 1 : 0);
    if (product_result.ok != kQolOk || !valid_craft_item(product)) {
        discard_uncommitted_product(product, "product_create", 0);
        return CraftExecuteResult::kProductPrepareFailed;
    }

    if (recipe.product_mode == ProductMode::kScaleFirstItem) {
        const int new_value = scaled_jewel_value(
            static_cast<int>(source_jewel_word & kJewelValueMask), recipe.scale_permille);
        const uint32_t product_word = (source_jewel_word & ~kJewelValueMask) |
                                      (static_cast<uint32_t>(new_value) & kJewelValueMask);
        const QolResult configured = g_write_port->write_jewel_word(product, product_word);
        log_execute_stage(configured.ok == kQolOk ? 0 : 2, "product_scale", 0,
                          configured.code, static_cast<int32_t>(source_jewel_word & kJewelValueMask),
                          new_value);
        if (configured.ok != kQolOk) {
            discard_uncommitted_product(product, "product_scale", 0);
            return CraftExecuteResult::kProductPrepareFailed;
        }
    }
    if (recipe.product_mode == ProductMode::kMaxSocketEnchantFirstItem) {
        const QolResult configured = g_write_port->set_max_socket_enchant(product);
        log_execute_stage(configured.ok == kQolOk ? 0 : 2, "product_max_socket", 0,
                          configured.code, 0, 1);
        if (configured.ok != kQolOk) {
            discard_uncommitted_product(product, "product_max_socket", 0);
            return CraftExecuteResult::kProductPrepareFailed;
        }
    }

    // 仍经原版 inventory entry 扣料，以保留 H-03/H-05/H-21 的扩展背包路径。
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        if (!valid_craft_item(items[i])) continue;
        CategoryPlan& category_plan = category_plans[category_for_slot[i]];
        const int32_t before = category_plan.current_count;
        const QolResult removed = stackable[i]
            ? g_game->inventory_write.remove_item_data(categories[i], 1)
            : g_game->inventory_write.remove_item(items[i]);
        int32_t after = -1;
        const QolResult observed = read_live_category_count(categories[i], &after);
        if (removed.ok != kQolOk) {
            log_execute_stage(2, "material_remove", static_cast<uint32_t>(i),
                              removed.code, before, after);
            if (observed.ok != kQolOk) {
                log_execute_stage(2, "material_observation", static_cast<uint32_t>(i),
                                  observed.code, before, after);
            }
            discard_uncommitted_product(product, "material_remove", static_cast<uint32_t>(i));
            return CraftExecuteResult::kMaterialRemoveFailed;
        }
        if (observed.ok != kQolOk) {
            log_execute_stage(2, "material_observation", static_cast<uint32_t>(i),
                              observed.code, before, after);
            discard_uncommitted_product(product, "material_observation", static_cast<uint32_t>(i));
            return CraftExecuteResult::kMaterialObservationFailed;
        }
        if (after != before - 1) {
            log_execute_stage(2, "material_observation", static_cast<uint32_t>(i),
                              kQolErrConflict, before, after);
            discard_uncommitted_product(product, "material_observation", static_cast<uint32_t>(i));
            return CraftExecuteResult::kMaterialObservationFailed;
        }
        category_plan.current_count = after;
        log_execute_stage(0, "material_remove", static_cast<uint32_t>(i), kQolOk, before, after);
    }

    // save_item may consume/rebind the handle; never free or inspect it after this call.
    int32_t saved = 0;
    const QolResult save_result = g_write_port->save_item(product, &saved);
    const bool product_saved = save_result.ok == kQolOk &&
                               (static_cast<uint32_t>(saved) & 0xffu) != 0;
    log_execute_stage(product_saved ? 0 : 2, "product_save", 0,
                      save_result.ok == kQolOk && !product_saved ? kQolErrInternal
                                                                  : save_result.code,
                      0, saved);
    if (!product_saved) {
        // save 可能已把 handle 交给 registry 或扩展背包，此处不得再 free 或回读该 handle。
        log_message(2, "three slot execute stage=product_save index=0 result=%d "
                       "after=ownership_unknown",
                    save_result.ok == kQolOk ? kQolErrInternal : save_result.code);
        return CraftExecuteResult::kProductSaveFailed;
    }
    if (g_verify_log_budget.fetch_sub(1) > 0) {
        log_message(0, "three slot crafted product=%u consumed=%d",
                    static_cast<uint32_t>(product_category),
                    valid_craft_item(items[0]) + valid_craft_item(items[1]) +
                        valid_craft_item(items[2]));
    }
    return CraftExecuteResult::kOk;
}

void api_material_needs(const result::CraftMaterialNeedInput* in,
                        result::CraftMaterialNeedOutput* out) {
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

int32_t api_make_item(const result::CraftMakeItemInput* in) {
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

    if (g_write_port == nullptr || g_write_port->write_jewel_word == nullptr) {
        log_message(2, "make item aborted: jewel writer unavailable");
        return 1;
    }
    MaterialNeedPlan material_plan{};
    const QolResult material_result = prepare_material_needs(def, in->item, &material_plan);
    if (material_result.ok != kQolOk) {
        log_message(2, "make item aborted: material precheck failed code=%d",
                    material_result.code);
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

    // UseStuff 在 MakeItem 返回 0 后读取 stuffList；失败时已经写入的显示值允许残留。
    const QolResult needs_written = write_material_needs(def, material_plan);
    if (needs_written.ok != kQolOk) return 1;

    // jewel write 是最后一个提交步骤；前序 quality/evaluate 错误不会改 stuffList 或宝石。
    const uint32_t updated_word =
        (word & ~kJewelValueMask) | (static_cast<uint32_t>(new_value) & kJewelValueMask);
    const QolResult jewel_written = g_write_port->write_jewel_word(in->item, updated_word);
    if (jewel_written.ok != kQolOk) {
        log_message(2, "make item failed stage=jewel_write result=%d needs=already_written",
                    jewel_written.code);
        return 1;
    }
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

const result::CraftResultApi kCraftResultApi = {
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
    g_ownership_ready = false;
    if (::custom_recipe::custom_recipe_table_ensure()) {
        g_ownership_ready = true;
        return 0;
    }
    ::custom_recipe::custom_recipe_table_deactivate();
    return 1;
}

void own_release(const char* /*id*/) {
    g_ownership_ready = false;
    ::custom_recipe::custom_recipe_table_deactivate();  // total，永不失败
}

const qol::contracts::OwnershipOps kOwnershipOps = {&own_acquire, &own_release};

}  // namespace

bool enabled() { return g_custom_recipe_enabled.load(std::memory_order_acquire); }

void cancel_pending_confirmation() {
    // 与停用同一条作废路径：丢弃 pending recipe/identity 并递增 token，
    // 使任何已排队的旧确认无法再执行；不触碰已填格材料或产物。
    three_slot_invalidate_pending();
}

QolResult random_inclusive(int32_t lo, int32_t hi, int32_t* out) {
    return port_random_inclusive(lo, hi, out);
}

bool ownership_ready() { return g_ownership_ready; }

bool set_enabled(bool enabled_value) {
    g_custom_recipe_enabled.store(enabled_value, std::memory_order_release);
    if (!enabled_value) {
        // 作废任何在途确认（残留面板上的 YesNo）。
        three_slot_invalidate_pending();
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

const result::CraftResultApi& craft_result_api() { return kCraftResultApi; }

const qol::contracts::OwnershipOps& ownership_ops() { return kOwnershipOps; }

bool handle_write_stuff_need_command(const uint8_t* payload, uint32_t size) {
    if (!enabled() || payload == nullptr || size != sizeof(contracts::CraftWriteStuffNeedPayload) ||
        g_write_port == nullptr || g_write_port->write_stuff_need == nullptr) {
        return false;
    }
    contracts::CraftWriteStuffNeedPayload command{};
    std::memcpy(&command, payload, sizeof(command));
    return g_write_port->write_stuff_need(command.index, command.category, command.count).ok ==
           kQolOk;
}

bool handle_set_stuff_item_command(const uint8_t* payload, uint32_t size) {
    if (!enabled() || payload == nullptr || size != sizeof(contracts::CraftSetStuffItemPayload) ||
        g_write_port == nullptr || g_write_port->set_stuff_item == nullptr) {
        return false;
    }
    contracts::CraftSetStuffItemPayload command{};
    std::memcpy(&command, payload, sizeof(command));
    return g_write_port->set_stuff_item(command.index, command.item).ok == kQolOk;
}

}  // namespace qol::features::custom_recipe
