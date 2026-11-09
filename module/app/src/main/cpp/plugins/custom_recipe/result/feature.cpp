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
#include <limits>

#include "../../../contracts/attribute_quality.h"
#include "../../../contracts/events.h"
#include "../../../contracts/plugin.h"
#include "../model/catalog.h"
#include "../model/rules.h"
#include "../table/table.h"

namespace qol::features::custom_recipe {

namespace {

constexpr uint32_t kJewelValueMask = 2047;
constexpr uint32_t kJewelTypeShift = 18;
constexpr uint32_t kJewelTypeMask = 63;

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolEventStateApi* g_event_state = nullptr;
const contracts::QolGameHost* g_game = nullptr;
const contracts::ItemFactsApi* g_item_facts = nullptr;
const contracts::CraftFactsApi* g_craft_facts = nullptr;
const contracts::SaveFactsApi* g_save_facts = nullptr;
const contracts::QolCustomRecipeWritePort* g_write_port = nullptr;
const contracts::InventoryActionApi* g_inventory_action = nullptr;
const contracts::QolInventorySlotApi* g_inventory_slot = nullptr;
const contracts::QolBagSourceRegistryApi* g_bag_source_registry = nullptr;
const contracts::ItemPayloadApi* g_item_payload = nullptr;
const contracts::QolInventoryTransferApi* g_inventory_transfer = nullptr;
uint64_t g_save_ready_subscription = 0;
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

void set_save_facts(const contracts::SaveFactsApi* facts) { g_save_facts = facts; }

void set_write_port(const contracts::QolCustomRecipeWritePort* port) { g_write_port = port; }

void set_inventory_action(const contracts::InventoryActionApi* api) {
    g_inventory_action = api;
}

void set_inventory_slot(const contracts::QolInventorySlotApi* api) { g_inventory_slot = api; }

void set_bag_source_registry(const contracts::QolBagSourceRegistryApi* api) {
    g_bag_source_registry = api;
}

void set_item_payload(const contracts::ItemPayloadApi* api) { g_item_payload = api; }

void set_inventory_transfer(const contracts::QolInventoryTransferApi* api) {
    g_inventory_transfer = api;
}

namespace {

QolResult update_item_payload(qol::contracts::QolItemHandle source,
                              const contracts::QolItemPayloadMutation* mutations,
                              uint32_t mutation_count, contracts::QolItemPayload* out) {
    if (g_item_payload == nullptr || g_item_payload->update == nullptr) {
        return recipe_error(kQolErrUnsupported, "item.payload capability unavailable");
    }
    contracts::QolItemPayloadUpdateSpec spec{};
    spec.struct_size = sizeof(spec);
    spec.source = source;
    spec.mutations = mutations;
    spec.mutation_count = mutation_count;
    spec.flags = contracts::kItemPayloadUpdateValidateStructure;
    return g_item_payload->update(&spec, out);
}

QolResult replace_item_from_payload(qol::contracts::QolItemHandle expected,
                                    const contracts::QolItemPayload& payload,
                                    contracts::QolBagMutationResult* out) {
    if (g_inventory_transfer == nullptr || g_inventory_transfer->replace_item == nullptr) {
        return recipe_error(kQolErrUnsupported, "inventory.transfer capability unavailable");
    }
    contracts::QolBagTransfer transfer{};
    transfer.category = static_cast<int32_t>(payload.category);
    transfer.count = static_cast<int32_t>(payload.count);
    transfer.payload_size = payload.payload_size;
    std::memcpy(transfer.payload, payload.payload, payload.payload_size);
    return g_inventory_transfer->replace_item(expected, &transfer, out);
}

}  // namespace

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
::custom_recipe::DynamicRecipeCache g_dynamic_recipe_cache{};

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
// kind == kTierUp（与总开关无关，停用后仍需识别以 fail-closed）。
const ::custom_recipe::Def* module_recipe_for_mix_type(uint32_t mix_type) {
    const ::custom_recipe::Def* def = ::custom_recipe::def_for_mix_type(mix_type);
    if (def == nullptr || def->kind != ::custom_recipe::Kind::kTierUp) return nullptr;
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

bool valid_item_type(qol::contracts::QolItemType type) {
    using qol::contracts::QolItemType;
    switch (type) {
        case QolItemType::kUnknown:
        case QolItemType::kConsumable:
        case QolItemType::kPotion:
        case QolItemType::kDice:
        case QolItemType::kReviveScroll:
        case QolItemType::kLamp:
        case QolItemType::kUseAfterConfirm:
        case QolItemType::kEquipment:
        case QolItemType::kBackpack:
        case QolItemType::kJewel:
        case QolItemType::kEnchantScroll:
        case QolItemType::kMercenarySeal:
        case QolItemType::kSkillBook:
        case QolItemType::kItemBox:
        case QolItemType::kSynthSet:
        case QolItemType::kOther:
            return true;
    }
    return false;
}

QolResult read_item_type(qol::contracts::QolItemHandle item,
                         qol::contracts::QolItemType* out_type) {
    if (!valid_craft_item(item) || out_type == nullptr) {
        return recipe_error(kQolErrInvalidArgument, "invalid item type input");
    }
    *out_type = qol::contracts::QolItemType::kUnknown;
    if (g_item_facts == nullptr || g_item_facts->item_type == nullptr) {
        return recipe_error(kQolErrNotReady, "item type facts unavailable");
    }
    const QolResult result = g_item_facts->item_type(item, out_type);
    if (result.ok != kQolOk) return result;
    if (!valid_item_type(*out_type)) {
        *out_type = qol::contracts::QolItemType::kUnknown;
        return recipe_error(kQolErrInternal, "invalid item type fact");
    }
    return contracts::ok();
}

bool item_category_matches(qol::contracts::QolItemHandle item, uint16_t expected) {
    int32_t category = 0;
    return valid_craft_item(item) && g_item_facts != nullptr &&
           g_item_facts->category != nullptr &&
           g_item_facts->category(item, &category).ok == kQolOk && category > 0 &&
           category == expected;
}

bool item_is_jewel(qol::contracts::QolItemHandle item) {
    qol::contracts::QolItemType type = qol::contracts::QolItemType::kUnknown;
    return read_item_type(item, &type).ok == kQolOk &&
           type == qol::contracts::QolItemType::kJewel;
}

QolResult query_jewel_fact(qol::contracts::QolItemHandle item, uint16_t* out_category,
                           bool* out_is_jewel) {
    if (!valid_craft_item(item) || out_category == nullptr || out_is_jewel == nullptr ||
        g_item_facts == nullptr || g_item_facts->category == nullptr ||
        g_item_facts->item_type == nullptr) {
        return recipe_error(kQolErrNotReady, "jewel item facts unavailable");
    }
    int32_t category = 0;
    QolResult result = g_item_facts->category(item, &category);
    if (result.ok != kQolOk) return result;
    if (category <= 0 || category > UINT16_MAX) {
        return recipe_error(kQolErrInternal, "invalid item category");
    }
    qol::contracts::QolItemType item_type = qol::contracts::QolItemType::kUnknown;
    result = read_item_type(item, &item_type);
    if (result.ok != kQolOk) return result;
    *out_category = static_cast<uint16_t>(category);
    *out_is_jewel = item_type == qol::contracts::QolItemType::kJewel;
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

// 升阶材料消耗的两项输入：品质档位（宝石类别 / 装备稀有度）与属性等阶
// （宝石 quality.level+1 / 装备可升阶词条最低 level+1）。
// 面板显示（api_material_needs）用本函数；执行路径（api_make_item）从已查询的
// quality / 候选词条取同一语义的输入，避免面板显示与实扣口径分叉。
QolResult tier_up_cost_inputs_for_item(qol::contracts::QolItemHandle item,
                                       int* out_quality_tier, int* out_attribute_tier) {
    if (out_quality_tier == nullptr || out_attribute_tier == nullptr) {
        return recipe_error(kQolErrInvalidArgument, "null tier-up cost input output");
    }
    *out_quality_tier = -1;
    *out_attribute_tier = -1;
    uint16_t category = 0;
    bool is_jewel = false;
    QolResult result = query_jewel_fact(item, &category, &is_jewel);
    if (result.ok != kQolOk) return result;
    if (is_jewel) {
        *out_quality_tier =
            ::custom_recipe::jewel_quality_tier_from_category(static_cast<int>(category));
        const qol::contracts::AttributeQualityApi* api = nullptr;
        qol::contracts::AttributeQualityItem quality{};
        result = query_jewel_quality(item, &api, &quality);
        if (result.ok != kQolOk) return result;
        *out_attribute_tier =
            ::custom_recipe::attribute_tier_from_quality_level(quality.quality.level);
        if (*out_quality_tier < 0 || *out_attribute_tier < 0) {
            return recipe_error(kQolErrNotReady, "jewel tier-up cost inputs unavailable");
        }
        return contracts::ok();
    }
    qol::contracts::QolItemType item_type = qol::contracts::QolItemType::kUnknown;
    result = read_item_type(item, &item_type);
    if (result.ok != kQolOk) return result;
    if (item_type != qol::contracts::QolItemType::kEquipment) {
        return recipe_error(kQolErrInvalidArgument, "tier-up target is not jewel or equipment");
    }
    if (g_item_facts->view == nullptr) {
        return recipe_error(kQolErrNotReady, "equipment inventory view unavailable");
    }
    qol::contracts::QolItemInventoryView view{};
    result = g_item_facts->view(item, &view);
    if (result.ok != kQolOk) return result;
    if ((view.fields & qol::contracts::kQolItemInventoryHasRarity) == 0) {
        return recipe_error(kQolErrNotReady, "equipment rarity unavailable");
    }
    if ((view.fields & qol::contracts::kQolItemInventoryHasNativeOptions) == 0 ||
        (view.fields & qol::contracts::kQolItemInventoryOptionsTruncated) != 0 ||
        view.native_option_count == 0 ||
        view.native_option_count > qol::contracts::kQolItemInventoryNativeOptionCapacity) {
        return recipe_error(kQolErrNotReady, "equipment native options unavailable");
    }
    *out_quality_tier = ::custom_recipe::equipment_quality_tier_from_rarity(view.rarity);
    const qol::contracts::AttributeQualityApi* api = nullptr;
    result = require_attribute_quality_api(&api);
    if (result.ok != kQolOk) return result;
    // 可升阶词条（type 0、status ok、level 0..4、区间非退化）中取最低 level 作等阶输入。
    int min_level = -1;
    for (uint32_t i = 0; i < view.native_option_count; ++i) {
        const qol::contracts::QolItemInventoryOptionView& option = view.native_options[i];
        if (option.type != static_cast<int32_t>(
                               qol::contracts::QolItemInventoryOptionType::kNative)) {
            continue;
        }
        qol::contracts::AttributeQualityItem context{};
        const QolResult queried =
            api->query_option_context(item, option.id, option.value, nullptr, &context);
        if (queried.ok != kQolOk ||
            !::custom_recipe::affix_upgradable(context.status, context.quality.level,
                                               context.range)) {
            continue;
        }
        if (min_level < 0 || context.quality.level < min_level) min_level = context.quality.level;
    }
    if (min_level < 0) {
        return recipe_error(kQolErrNotFound, "no upgradable equipment affix");
    }
    *out_attribute_tier = ::custom_recipe::attribute_tier_from_quality_level(min_level);
    if (*out_quality_tier < 0 || *out_attribute_tier < 0) {
        return recipe_error(kQolErrNotReady, "equipment tier-up cost inputs unavailable");
    }
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
    int quality_tier = 0;
    int attribute_tier = 0;
    int32_t character_level = 0;
};

// 先完整验证需求，再由调用方依序写入 stuffList。
QolResult prepare_material_needs(const ::custom_recipe::Def* def, int quality_tier,
                                 int attribute_tier, MaterialNeedPlan* out_plan) {
    if (def == nullptr || out_plan == nullptr || quality_tier < 0 || quality_tier > 4 ||
        attribute_tier < 0 || attribute_tier > 4 ||
        def->count_rule != ::custom_recipe::CountRule::kQualityAndAttributeTier ||
        def->materials == nullptr || def->material_count == 0 || def->material_count > 8) {
        return recipe_error(kQolErrInvalidArgument, "invalid tier-up material definition");
    }
    if (g_craft_facts == nullptr || g_craft_facts->read.character_level == nullptr) {
        return recipe_error(kQolErrNotReady, "character level facts unavailable");
    }
    int32_t level = 0;
    QolResult result = g_craft_facts->read.character_level(&level);
    if (result.ok != kQolOk) return result;
    if (g_write_port == nullptr || g_write_port->write_stuff_need == nullptr) {
        return recipe_error(kQolErrNotReady, "stuff need writer unavailable");
    }

    MaterialNeedPlan plan{};
    plan.count = def->material_count;
    plan.quality_tier = quality_tier;
    plan.attribute_tier = attribute_tier;
    plan.character_level = level;
    for (uint8_t i = 0; i < def->material_count; ++i) {
        // 条目数据仍须合法（item_id/count 非零）；消耗数量只由品质+等阶+等级决定，
        // 不随条目 count 缩放（升阶材料以 1 瓶为基准）。
        if (def->materials[i].item_id == 0 || def->materials[i].count == 0) {
            return recipe_error(kQolErrInternal, "invalid tier-up material entry");
        }
        plan.needs[i] = ::custom_recipe::material_count_for(quality_tier, attribute_tier, level);
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
        log_message(0,
                    "material needs quality_tier=%d attribute_tier=%d level=%d count=%u first=%d",
                    plan.quality_tier, plan.attribute_tier, plan.character_level,
                    static_cast<uint32_t>(plan.count), plan.needs[0]);
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

class MakeItemInputScope {
public:
    explicit MakeItemInputScope(const result::CraftMakeItemInput* input) {
        g_make_item_input = input;
    }
    ~MakeItemInputScope() { g_make_item_input = nullptr; }
    MakeItemInputScope(const MakeItemInputScope&) = delete;
    MakeItemInputScope& operator=(const MakeItemInputScope&) = delete;
};

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

QolResult build_dynamic_recipe_cache_miss(uint64_t seed,
                                          ::custom_recipe::ThreeSlotRecipe* out,
                                          size_t out_capacity, size_t* out_count,
                                          void* context) {
    auto* reroll_count = static_cast<size_t*>(context);
    return ::custom_recipe::build_dynamic_recipes_seeded(
        ::custom_recipe::kSpecialEquipCategories, ::custom_recipe::kSpecialEquipCategoryCount,
        seed, ::custom_recipe::kMaterialPool, ::custom_recipe::kMaterialPoolSize, out,
        out_capacity, out_count, reroll_count);
}

void refresh_dynamic_recipes(void* /*ctx*/) {
    three_slot_invalidate_pending();
    if (g_save_facts == nullptr || g_save_facts->current_slot == nullptr ||
        g_save_facts->slot_times == nullptr) {
        ::custom_recipe::set_dynamic_three_slot_recipes(nullptr, 0);
        log_message(2, "dynamic recipe rebuild skipped: save facts unavailable");
        return;
    }

    int32_t slot = -1;
    const QolResult slot_result = g_save_facts->current_slot(&slot);
    if (slot_result.ok != kQolOk || slot < 0 || slot >= 3) {
        ::custom_recipe::set_dynamic_three_slot_recipes(nullptr, 0);
        log_message(2, "dynamic recipe rebuild skipped: current slot invalid result=%d slot=%d",
                    slot_result.code, slot);
        return;
    }

    uint64_t creation_time_ms = 0;
    uint64_t save_time_ms = 0;
    const QolResult time_result =
        g_save_facts->slot_times(slot, &creation_time_ms, &save_time_ms);
    (void)save_time_ms;
    if (time_result.ok != kQolOk || creation_time_ms == 0) {
        ::custom_recipe::set_dynamic_three_slot_recipes(nullptr, 0);
        log_message(2, "dynamic recipe rebuild skipped: creation time unavailable slot=%d result=%d",
                    slot, time_result.code);
        return;
    }

    ::custom_recipe::ThreeSlotRecipe buffer[::custom_recipe::kMaxDynamicRecipes];
    size_t n = 0;
    size_t rerolls = 0;
    bool cache_hit = false;
    const QolResult result = ::custom_recipe::dynamic_recipe_cache_get_or_build(
        &g_dynamic_recipe_cache, creation_time_ms, &build_dynamic_recipe_cache_miss, &rerolls,
        buffer, ::custom_recipe::kMaxDynamicRecipes, &n, &cache_hit);
    if (result.ok != kQolOk) {
        ::custom_recipe::set_dynamic_three_slot_recipes(nullptr, 0);
        log_message(2, "dynamic recipe rebuild failed stage=recipe_cache result=%d",
                    result.code);
        return;
    }
    ::custom_recipe::set_dynamic_three_slot_recipes(buffer, n);
    log_message(1, "dynamic special recipes refreshed count=%zu slot=%d cache=%s rerolls=%zu",
                n, slot, cache_hit ? "hit" : "miss", rerolls);
}

void refresh_dynamic_recipes_event(void* /*ctx*/, const contracts::EventStateChange* change) {
    if (change == nullptr ||
        change->availability != static_cast<uint8_t>(contracts::EventStateAvailability::kAvailable) ||
        change->value != 1) return;
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
        g_item_facts->item_type == nullptr) {
        return recipe_error(kQolErrNotReady, "item stackability facts unavailable");
    }
    qol::contracts::QolItemType item_type = qol::contracts::QolItemType::kUnknown;
    const QolResult result = read_item_type(item, &item_type);
    if (result.ok != kQolOk) return result;
    *out_stackable = item_type != qol::contracts::QolItemType::kEquipment;
    return contracts::ok();
}

void set_jewel_slots_from_items(
    const uint16_t categories[kThreeSlotStuffCount],
    const qol::contracts::QolItemHandle items[kThreeSlotStuffCount],
    bool out_jewel_slots[kThreeSlotStuffCount]) {
    if (out_jewel_slots == nullptr) return;
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        out_jewel_slots[i] = false;
        if (categories == nullptr || categories[i] == 0 || items == nullptr ||
            !valid_craft_item(items[i])) {
            continue;
        }
        qol::contracts::QolItemType item_type = qol::contracts::QolItemType::kUnknown;
        if (read_item_type(items[i], &item_type).ok == kQolOk) {
            out_jewel_slots[i] = item_type == qol::contracts::QolItemType::kJewel;
        }
    }
}

void read_slot_jewel_types(const uint16_t categories[kThreeSlotStuffCount],
                           bool out_jewel_slots[kThreeSlotStuffCount]) {
    qol::contracts::QolItemHandle items[kThreeSlotStuffCount] = {
        qol::contracts::kInvalidItemHandle,
        qol::contracts::kInvalidItemHandle,
        qol::contracts::kInvalidItemHandle,
    };
    if (g_craft_facts != nullptr && g_craft_facts->read.stuff_item != nullptr) {
        for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
            if (categories == nullptr || categories[i] == 0) continue;
            (void)g_craft_facts->read.stuff_item(static_cast<uint32_t>(i), &items[i]);
        }
    }
    set_jewel_slots_from_items(categories, items, out_jewel_slots);
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

int32_t inventory_action_status_code(contracts::InventoryActionStatus status) {
    switch (status) {
        case contracts::InventoryActionStatus::kOk:
            return kQolOk;
        case contracts::InventoryActionStatus::kNotInWorld:
            return kQolErrNotReady;
        case contracts::InventoryActionStatus::kInvalidRef:
            return kQolErrInvalidArgument;
        case contracts::InventoryActionStatus::kStale:
        case contracts::InventoryActionStatus::kEmpty:
            return kQolErrNotFound;
        case contracts::InventoryActionStatus::kTaskBagExcluded:
        case contracts::InventoryActionStatus::kUnsupported:
            return kQolErrUnsupported;
        case contracts::InventoryActionStatus::kConflict:
            return kQolErrConflict;
        case contracts::InventoryActionStatus::kFull:
            return kQolErrBlocked;
        default:
            return kQolErrInternal;
    }
}

// 注册袋源（扩展背包）物品的 view 袋号是「原版 + 改版 + 各注册源」串联后的展示键，
// 不含空间身份，无法当成 QolActionRef；这里按句柄在注册源内定位，拿到显式 space/bag/slot。
bool locate_registered_action_ref(contracts::QolItemHandle item,
                                  contracts::QolActionRef* out_ref) {
    if (out_ref == nullptr || g_bag_source_registry == nullptr ||
        g_bag_source_registry->source_count == nullptr ||
        g_bag_source_registry->source_at == nullptr) {
        return false;
    }
    const uint32_t source_count = g_bag_source_registry->source_count();
    for (uint32_t index = 0; index < source_count; ++index) {
        const contracts::QolBagSource* source = nullptr;
        if (!contracts::succeeded(g_bag_source_registry->source_at(index, &source)) ||
            source == nullptr || source->item_at == nullptr) {
            continue;
        }
        // identify 是可选原语：命中后仍用 item_at 复核句柄，避免把过期定位当成结果。
        if (source->identify != nullptr) {
            contracts::QolBagRef ref{};
            int32_t slot = -1;
            if (contracts::succeeded(source->identify(item, &ref, &slot)) &&
                ref.space_id == source->space_id && ref.bag >= 0 && slot >= 0) {
                contracts::QolInventoryItemRef located{};
                if (contracts::succeeded(source->item_at(ref.bag, slot, &located)) &&
                    same_item_handle(located.item, item)) {
                    *out_ref = {source->space_id, ref.bag, slot};
                    return true;
                }
            }
        }
        if (source->count == nullptr || source->info == nullptr) continue;
        const int32_t bag_count = source->count();
        if (bag_count < 0) continue;
        for (int32_t bag = 0; bag < bag_count; ++bag) {
            contracts::QolBagInfo info{};
            if (!contracts::succeeded(source->info(bag, &info)) || info.capacity < 0) continue;
            for (int32_t slot = 0; slot < info.capacity; ++slot) {
                contracts::QolInventoryItemRef located{};
                if (!contracts::succeeded(source->item_at(bag, slot, &located))) continue;
                if (!same_item_handle(located.item, item)) continue;
                *out_ref = {source->space_id, bag, slot};
                return true;
            }
        }
    }
    return false;
}

// handle → 显式袋引用。inventory.slot 的 view 按扁平袋号上报位置：原版空间固定 6 袋
// （item_port.h kQolOriginalBagCount），因此扁平袋号 < 6 属原版空间，其余原生物品属改版
// 空间且袋号需减去该偏移。扩展袋物品按句柄在注册源内定位（见上）。
bool locate_material_action_ref(contracts::QolItemHandle item, contracts::QolActionRef* out_ref) {
    if (out_ref == nullptr || g_inventory_slot == nullptr || g_inventory_slot->view == nullptr) {
        return false;
    }
    contracts::QolInventorySlotView view{};
    view.struct_size = sizeof(view);
    if (!contracts::succeeded(g_inventory_slot->view(item, &view))) return false;
    if (view.item.slot != item.slot || view.item.generation != item.generation) return false;
    if (view.bag < 0 || view.slot < 0) return false;
    if (view.kind == static_cast<uint32_t>(contracts::QolInventoryItemKind::kProvider)) {
        return locate_registered_action_ref(item, out_ref);
    }
    if (view.kind != static_cast<uint32_t>(contracts::QolInventoryItemKind::kOriginal)) return false;
    contracts::QolActionRef ref{contracts::kQolBagSpaceOriginal, view.bag, view.slot};
    if (view.bag >= contracts::kQolOriginalBagCount) {
        ref.space_id = contracts::kQolBagSpaceVariant;
        ref.bag = view.bag - contracts::kQolOriginalBagCount;
    }
    if (ref.bag < 0) return false;
    // inventory.action 明确拒绝任务袋，而原版类别入口遍历袋 0..5；保留下沉路径而不是新增失败。
    if (ref.space_id == contracts::kQolBagSpaceOriginal && ref.bag == contracts::kQolTaskBagIndex) {
        return false;
    }
    *out_ref = ref;
    return true;
}

void log_execute_stage(int32_t level, const char* stage, uint32_t index,
                       int32_t result, int32_t before, int32_t after) {
    if (level == 0 && g_verify_log_budget.fetch_sub(1) <= 0) return;
    log_message(level,
                "three slot execute stage=%s index=%u result=%d before=%d after=%d",
                stage, index, result, before, after);
}

// 扣料成功行：stage 名与字段顺序与 log_execute_stage 一致，追加 path 区分句柄锚定扣减
// （discard）、原版类别回退（category_fallback）与不可堆叠单件删除（remove_item）。
void log_material_remove(uint32_t index, const char* path, int32_t before, int32_t after) {
    if (g_verify_log_budget.fetch_sub(1) <= 0) return;
    log_message(0,
                "three slot execute stage=material_remove index=%u result=%d before=%d "
                "after=%d path=%s",
                index, kQolOk, before, after, path);
}

void discard_uncommitted_product(qol::contracts::QolItemHandle product,
                                 const char* failed_stage, uint32_t index) {
    if (!valid_craft_item(product)) return;
    if (g_inventory_action == nullptr || g_inventory_action->discard_handle == nullptr) {
        log_message(2, "three slot execute stage=product_cleanup index=%u result=%d "
                       "after=%s ownership=unknown",
                    index, kQolErrNotReady, failed_stage);
        return;
    }
    const qol::contracts::InventoryUseOutcome discarded =
        g_inventory_action->discard_handle(product, 1);
    if (discarded.status != qol::contracts::InventoryActionStatus::kOk) {
        log_message(2, "three slot execute stage=product_cleanup index=%u result=%d "
                       "after=%s ownership=unknown",
                    index, inventory_action_status_code(discarded.status), failed_stage);
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

bool api_is_tier_up_recipe(uint32_t mix_type) {
    return module_recipe_for_mix_type(mix_type) != nullptr;
}

int64_t api_form_ui_type_for_mix_type(uint32_t mix_type) {
    // 停用后不再映射到模块借用 type：残留面板上的配方点击不得触发 SetType 换型。
    if (!enabled()) return kNoBorrowedType;
    const ::custom_recipe::Def* def = ::custom_recipe::def_for_mix_type(mix_type);
    if (def == nullptr) return kNoBorrowedType;
    return form_borrowed_type(def->form);
}

// 升阶门禁诊断：每次点击最多两条（失败/通过），进程内上限 64 条。
// 门禁共有 8 个返回 kUnavailable 的分支，界面侧只看到同一句「功能不可用」；
// 没有这条日志就无法从设备日志区分目标读取、品质能力、词条一致性等原因。
std::atomic<int> g_gate_log_budget{64};

bool gate_log_take() { return g_gate_log_budget.fetch_sub(1) > 0; }

void gate_log_blocked(const char* stage, int32_t code, qol::contracts::QolItemHandle item) {
    if (!gate_log_take()) return;
    log_message(2, "tier-up gate blocked stage=%s code=%d handle=%u/%u", stage, code,
                static_cast<uint32_t>(item.slot), static_cast<uint32_t>(item.generation));
}

result::CraftGate api_craft_gate(qol::contracts::QolItemHandle target_item) {
    using result::CraftGate;
    uint16_t category = 0;
    bool is_jewel = false;
    const QolResult target_facts = query_jewel_fact(target_item, &category, &is_jewel);
    if (target_facts.ok != kQolOk) {
        gate_log_blocked("target_facts", target_facts.code, target_item);
        return CraftGate::kUnavailable;
    }
    if (is_jewel) {
        const qol::contracts::AttributeQualityApi* api = nullptr;
        qol::contracts::AttributeQualityItem quality{};
        const QolResult quality_result = query_jewel_quality(target_item, &api, &quality);
        if (quality_result.ok != kQolOk) {
            gate_log_blocked("jewel_quality", quality_result.code, target_item);
            return CraftGate::kUnavailable;
        }
        uint32_t word = 0;
        if (!read_jewel_word(target_item, &word)) {
            gate_log_blocked("jewel_word_read", kQolErrNotReady, target_item);
            return CraftGate::kUnavailable;
        }
        const int32_t word_attribute =
            static_cast<int32_t>((word >> kJewelTypeShift) & kJewelTypeMask);
        const int32_t word_value = static_cast<int32_t>(word & kJewelValueMask);
        if (quality.attribute_id != word_attribute || quality.value != word_value) {
            if (gate_log_take()) {
                log_message(2,
                            "tier-up gate blocked stage=jewel_word_mismatch quality=%d/%d "
                            "word=%d/%d handle=%u/%u",
                            quality.attribute_id, quality.value, word_attribute, word_value,
                            static_cast<uint32_t>(target_item.slot),
                            static_cast<uint32_t>(target_item.generation));
            }
            return CraftGate::kUnavailable;
        }
        if (quality.quality.level == qol::contracts::kAttributeQualityHighestLevel) {
            return CraftGate::kAlreadyGold;
        }
        if (gate_log_take()) {
            log_message(0, "tier-up gate pass kind=jewel category=%u level=%d", category,
                        quality.quality.level);
        }
        return CraftGate::kPass;
    }

    qol::contracts::QolItemType item_type = qol::contracts::QolItemType::kUnknown;
    const QolResult type_result = read_item_type(target_item, &item_type);
    if (type_result.ok != kQolOk) {
        gate_log_blocked("item_type", type_result.code, target_item);
        return CraftGate::kUnavailable;
    }
    if (item_type != qol::contracts::QolItemType::kEquipment) {
        if (gate_log_take()) {
            log_message(1, "tier-up gate blocked stage=not_equipment item_type=%d handle=%u/%u",
                        static_cast<int32_t>(item_type),
                        static_cast<uint32_t>(target_item.slot),
                        static_cast<uint32_t>(target_item.generation));
        }
        return CraftGate::kNotJewel;
    }
    if (g_item_facts->view == nullptr) {
        gate_log_blocked("view_unavailable", kQolErrNotReady, target_item);
        return CraftGate::kUnavailable;
    }
    qol::contracts::QolItemInventoryView view{};
    const QolResult view_result = g_item_facts->view(target_item, &view);
    if (view_result.ok != kQolOk) {
        gate_log_blocked("view", view_result.code, target_item);
        return CraftGate::kUnavailable;
    }
    if ((view.fields & qol::contracts::kQolItemInventoryHasNativeOptions) == 0 ||
        (view.fields & qol::contracts::kQolItemInventoryOptionsTruncated) != 0 ||
        view.native_option_count > qol::contracts::kQolItemInventoryNativeOptionCapacity) {
        if (gate_log_take()) {
            log_message(2,
                        "tier-up gate blocked stage=view_flags fields=0x%x native=%u jewel=%u "
                        "handle=%u/%u",
                        view.fields, view.native_option_count, view.jewel_option_count,
                        static_cast<uint32_t>(target_item.slot),
                        static_cast<uint32_t>(target_item.generation));
        }
        return CraftGate::kUnavailable;
    }
    if (view.native_option_count == 0) {
        if (gate_log_take()) {
            log_message(1, "tier-up gate blocked stage=no_native_option handle=%u/%u",
                        static_cast<uint32_t>(target_item.slot),
                        static_cast<uint32_t>(target_item.generation));
        }
        return CraftGate::kNoUpgradableAffix;
    }
    const qol::contracts::AttributeQualityApi* api = nullptr;
    const QolResult api_result = require_attribute_quality_api(&api);
    if (api_result.ok != kQolOk) {
        gate_log_blocked("quality_capability", api_result.code, target_item);
        return CraftGate::kUnavailable;
    }
    uint32_t upgradable = 0;
    for (uint32_t i = 0; i < view.native_option_count; ++i) {
        const qol::contracts::QolItemInventoryOptionView& option = view.native_options[i];
        if (option.type != static_cast<int32_t>(
                               qol::contracts::QolItemInventoryOptionType::kNative)) {
            continue;
        }
        qol::contracts::AttributeQualityItem context{};
        const QolResult queried = api->query_option_context(
            target_item, option.id, option.value, nullptr, &context);
        if (queried.ok == kQolOk &&
            ::custom_recipe::affix_upgradable(context.status, context.quality.level,
                                              context.range)) {
            ++upgradable;
        }
    }
    if (upgradable == 0) {
        if (gate_log_take()) {
            log_message(1, "tier-up gate blocked stage=no_upgradable_affix options=%u handle=%u/%u",
                        view.native_option_count, static_cast<uint32_t>(target_item.slot),
                        static_cast<uint32_t>(target_item.generation));
        }
        return CraftGate::kNoUpgradableAffix;
    }
    if (gate_log_take()) {
        log_message(0, "tier-up gate pass kind=equipment category=%u options=%u upgradable=%u",
                    category, view.native_option_count, upgradable);
    }
    return CraftGate::kPass;
}

bool api_is_jewel_item(qol::contracts::QolItemHandle item) { return item_is_jewel(item); }

// 主线程契约：仅日志级观测，不改变返回值（所有入口约定在游戏主线程执行）。
static void note_thread_contract(const char* entry) {
    if (g_game == nullptr || g_game->thread.is_main_thread == nullptr) return;
    if (g_game->thread.is_main_thread() != 0) return;
    static std::atomic<bool> reported{false};
    if (reported.exchange(true)) return;
    log_message(2, "wrong thread entry=%s (main-thread contract violated)", entry);
}

result::PlaceOutcome api_place(const result::CraftPlaceInput* in) {
    note_thread_contract("place");
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
    note_thread_contract("prepare");
    using result::PrepareOutcome;
    if (in == nullptr || !enabled()) return PrepareOutcome::kNotMine;
    three_slot_invalidate_pending();
    uint16_t categories[kThreeSlotStuffCount] = {in->slot_categories[0], in->slot_categories[1],
                                                 in->slot_categories[2]};
    bool jewel_slots[kThreeSlotStuffCount]{};
    read_slot_jewel_types(categories, jewel_slots);
    const ::custom_recipe::ThreeSlotRecipe* recipe =
        ::custom_recipe::match_three_slot(categories, &is_special_equip_category, jewel_slots);
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

result::CraftExecuteResult api_execute(const result::CraftExecuteInput* in,
                                       uint32_t* delegated_mix_type) {
    note_thread_contract("execute");
    using ::custom_recipe::ProductMode;
    using ::custom_recipe::ProductOrigin;
    using ::custom_recipe::ThreeSlotRecipe;
    using result::CraftExecuteResult;
    if (in == nullptr) return CraftExecuteResult::kAborted;

    const ThreeSlotRecipe recipe = g_pending_three_slot;
    const bool update_first = recipe.product_origin == ProductOrigin::kUpdateFirstItem;
    // 堆叠物品无法只升级其中一件；单件时才原地更新，堆叠时回退为新建产物。
    const bool update_in_place = update_first && in != nullptr && in->held_counts[0] == 1;
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

    bool jewel_slots[kThreeSlotStuffCount]{};
    set_jewel_slots_from_items(in->slot_categories, in->items, jewel_slots);
    const ThreeSlotRecipe* matched_recipe = ::custom_recipe::match_three_slot(
        in->slot_categories, &is_special_equip_category, jewel_slots);
    if (matched_recipe == nullptr || !same_three_slot_recipe(*matched_recipe, recipe)) {
        log_execute_stage(1, "precheck_recipe", 0, kQolErrConflict, 0, 0);
        return CraftExecuteResult::kStaleSlots;
    }

    // 三同类档位宝石三合一 → 委托该变体原版 UIMix_StartMix（产物词条含大修定向合成、
    // 扣料、清格、弹窗全部原版行为）。此判定须在 craft_facts/inventory_action 就绪检查之前：
    // 委托路径不使用任何插件产物/扣料端口。
    if (const uint32_t delegate = ::custom_recipe::delegate_mix_type_for(*matched_recipe,
                                                                        in->slot_categories);
        delegate != 0) {
        if (delegated_mix_type != nullptr) *delegated_mix_type = delegate;
        log_execute_stage(1, "delegate_original_mix", delegate, kQolOk, 0, 0);
        return CraftExecuteResult::kOkDelegated;
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
        // 两个分支都先按填入堆扣减（discard）；原版入口只是回退口，因此两者都要就绪。
        if (g_inventory_action == nullptr || g_inventory_action->discard == nullptr ||
            g_inventory_slot == nullptr || g_inventory_slot->view == nullptr) {
            log_execute_stage(2, "precheck_remove_ready", static_cast<uint32_t>(i),
                              kQolErrNotReady, 0, 0);
            return CraftExecuteResult::kPrecheckFailed;
        }
        if (stackable[i]) {
            // 可堆叠材料的回退口是原版类别入口（扩展袋补扣只在那里）。
            if (g_game == nullptr || g_game->inventory_write.remove_item_data == nullptr) {
                log_execute_stage(2, "precheck_remove_ready", static_cast<uint32_t>(i),
                                  kQolErrNotReady, 0, 0);
                return CraftExecuteResult::kPrecheckFailed;
            }
        } else {
            // 不可堆叠材料的回退口是按句柄删除：句柄失效即失败，绝不按类别扣同类。
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

    if (update_first && recipe.product_mode == ProductMode::kFixedCategory) {
        log_execute_stage(2, "precheck_update_first", 0, kQolErrInvalidArgument, 0, 0);
        return CraftExecuteResult::kPrecheckFailed;
    }

    if (g_write_port == nullptr || g_inventory_action == nullptr ||
        (!update_in_place && (g_inventory_action->create == nullptr ||
                              g_inventory_action->place == nullptr ||
                              g_inventory_action->discard_handle == nullptr)) ||
        ((recipe.product_mode == ProductMode::kScaleFirstItem ||
          recipe.product_mode == ProductMode::kMaxSocketEnchantFirstItem) &&
         (g_item_payload == nullptr || g_item_payload->update == nullptr)) ||
        (update_in_place &&
         (g_inventory_transfer == nullptr || g_inventory_transfer->replace_item == nullptr))) {
        log_execute_stage(2, "precheck_product_ready", 0, kQolErrNotReady, 0, 0);
        return CraftExecuteResult::kPrecheckFailed;
    }

    log_execute_stage(0, "precheck", 0, kQolOk, static_cast<int32_t>(category_count),
                      static_cast<int32_t>(kThreeSlotStuffCount));

    // 产物对象来源由配方声明：
    // - kNewItem：先创建尚未保存的 detached 产物，再按 ProductMode 配置；
    // - kUpdateFirstItem：产物就是第 1 格原物品，原地配置，不创建/不 place。
    qol::contracts::QolItemHandle product = qol::contracts::kInvalidItemHandle;
    if (update_in_place) {
        product = items[0];
        log_execute_stage(valid_craft_item(product) ? 0 : 2, "product_in_place", 0,
                          valid_craft_item(product) ? kQolOk : kQolErrNotFound, 0,
                          valid_craft_item(product) ? 1 : 0);
        if (!valid_craft_item(product)) {
            return CraftExecuteResult::kProductPrepareFailed;
        }
    } else {
        // perfect 不可用时回退普通创建，与迁移前的「perfect 端口缺失则 create_item」一致。
        qol::contracts::QolItemCreateSpec create_spec{
            sizeof(qol::contracts::QolItemCreateSpec), static_cast<int32_t>(product_category), 0,
            qol::contracts::kQolItemCreateFlagPerfect, nullptr, 0};
        qol::contracts::QolItemCreateOutcome created = g_inventory_action->create(&create_spec);
        if (created.status == qol::contracts::InventoryActionStatus::kUnsupported) {
            create_spec.flags = 0;
            created = g_inventory_action->create(&create_spec);
        }
        QolResult product_result = contracts::ok();
        if (created.status != qol::contracts::InventoryActionStatus::kOk) {
            static constexpr char kMessage[] = "product create failed";
            product_result = contracts::error(inventory_action_status_code(created.status),
                                              kMessage,
                                              static_cast<uint32_t>(sizeof(kMessage) - 1));
        } else {
            product = created.item;
        }
        log_execute_stage(product_result.ok == kQolOk && valid_craft_item(product) ? 0 : 2,
                          "product_create", 0,
                          product_result.ok == kQolOk && valid_craft_item(product)
                              ? kQolOk
                              : (product_result.ok == kQolOk ? kQolErrInternal
                                                             : product_result.code),
                          0, valid_craft_item(product) ? 1 : 0);
        if (product_result.ok != kQolOk || !valid_craft_item(product)) {
            discard_uncommitted_product(product, "product_create", 0);
            return CraftExecuteResult::kProductPrepareFailed;
        }
    }

    if (recipe.product_mode == ProductMode::kScaleFirstItem) {
        const int new_value = scaled_jewel_value(
            static_cast<int>(source_jewel_word & kJewelValueMask), recipe.scale_permille);
        const uint32_t product_word = (source_jewel_word & ~kJewelValueMask) |
                                      (static_cast<uint32_t>(new_value) & kJewelValueMask);
        const contracts::QolItemPayloadMutation mutation{
            static_cast<int32_t>(contracts::QolItemPayloadMutationKind::kJewelWord), 0,
            static_cast<int64_t>(product_word)};
        contracts::QolItemPayload payload{};
        const qol::contracts::QolItemHandle config_target = update_in_place ? items[0] : product;
        const QolResult configured = update_item_payload(config_target, &mutation, 1, &payload);
        log_execute_stage(configured.ok == kQolOk ? 0 : 2, "product_scale", 0,
                          configured.code, static_cast<int32_t>(source_jewel_word & kJewelValueMask),
                          new_value);
        if (configured.ok != kQolOk) {
            if (!update_in_place) discard_uncommitted_product(product, "product_scale", 0);
            return CraftExecuteResult::kProductPrepareFailed;
        }
        if (update_in_place) {
            contracts::QolBagMutationResult replaced{};
            const QolResult committed =
                replace_item_from_payload(config_target, payload, &replaced);
            log_execute_stage(committed.ok == kQolOk ? 0 : 2, "product_scale_commit", 0,
                              committed.code, 0, 1);
            if (committed.ok != kQolOk) return CraftExecuteResult::kProductPrepareFailed;
        }
    }
    if (recipe.product_mode == ProductMode::kMaxSocketEnchantFirstItem) {
        const contracts::QolItemPayloadMutation mutations[2] = {
            {static_cast<int32_t>(contracts::QolItemPayloadMutationKind::kSocketLevel), 0, 15},
            {static_cast<int32_t>(contracts::QolItemPayloadMutationKind::kEnchantRemaining), 0, 15},
        };
        contracts::QolItemPayload payload{};
        const qol::contracts::QolItemHandle config_target = update_in_place ? items[0] : product;
        const QolResult configured = update_item_payload(config_target, mutations, 2, &payload);
        log_execute_stage(configured.ok == kQolOk ? 0 : 2, "product_max_socket", 0,
                          configured.code, 0, 1);
        if (configured.ok != kQolOk) {
            if (!update_in_place) discard_uncommitted_product(product, "product_max_socket", 0);
            return CraftExecuteResult::kProductPrepareFailed;
        }
        if (update_in_place) {
            contracts::QolBagMutationResult replaced{};
            const QolResult committed =
                replace_item_from_payload(config_target, payload, &replaced);
            log_execute_stage(committed.ok == kQolOk ? 0 : 2, "product_max_socket_commit", 0,
                              committed.code, 0, 1);
            if (committed.ok != kQolOk) return CraftExecuteResult::kProductPrepareFailed;
        }
    }

    // 每格优先扣填入的那一堆：类别删除永远命中袋序第一堆，会把玩家填入的堆留下。
    // discard 带显式 space/bag/slot（由句柄定位），因此扣的是填入堆自身；两种堆叠形态同路。
    // 仅在填入堆已耗尽（句柄失效/堆已删除，连续合成的后续次数即如此）或物品不在 action
    // 可表示的空间时回退原版入口：可堆叠走类别删除（扩展袋补扣 H-21/R-56 只在那里，不能
    // 用枚举+discard 重写），不可堆叠走按句柄删除——句柄失效即失败，绝不按类别扣同类。
    for (size_t i = 0; i < kThreeSlotStuffCount; ++i) {
        // 更新原物品的配方：第 1 格是产物本体，只消耗其余材料。
        if (update_in_place && i == 0) continue;
        if (!valid_craft_item(items[i])) continue;
        CategoryPlan& category_plan = category_plans[category_for_slot[i]];
        const int32_t before = category_plan.current_count;
        contracts::QolActionRef ref{};
        bool used_discard = false;
        bool action_attempted = false;
        QolResult removed = contracts::ok();
        if (locate_material_action_ref(items[i], &ref)) {
            action_attempted = true;
            const contracts::InventoryUseOutcome discard = g_inventory_action->discard(ref, 1);
            if (discard.status == contracts::InventoryActionStatus::kOk &&
                discard.consumed_count == 1) {
                used_discard = true;
            } else {
                // 定位成功但扣减失败：不降级到类别扫描（那会扣到另一堆），直接按失败处理。
                removed = recipe_error(
                    discard.status == contracts::InventoryActionStatus::kOk
                        ? kQolErrInternal
                        : inventory_action_status_code(discard.status),
                    "material discard failed");
            }
        }
        if (!used_discard && !action_attempted) {
            removed = stackable[i]
                ? g_game->inventory_write.remove_item_data(categories[i], 1)
                : g_game->inventory_write.remove_item(items[i]);
        }
        int32_t after = -1;
        const QolResult observed = read_live_category_count(categories[i], &after);
        if (removed.ok != kQolOk) {
            log_execute_stage(2, "material_remove", static_cast<uint32_t>(i),
                              removed.code, before, after);
            if (observed.ok != kQolOk) {
                log_execute_stage(2, "material_observation", static_cast<uint32_t>(i),
                                  observed.code, before, after);
            }
            if (!update_in_place) {
                discard_uncommitted_product(product, "material_remove",
                                            static_cast<uint32_t>(i));
            }
            return CraftExecuteResult::kMaterialRemoveFailed;
        }
        if (observed.ok != kQolOk) {
            log_execute_stage(2, "material_observation", static_cast<uint32_t>(i),
                              observed.code, before, after);
            if (!update_in_place) {
                discard_uncommitted_product(product, "material_observation",
                                            static_cast<uint32_t>(i));
            }
            return CraftExecuteResult::kMaterialObservationFailed;
        }
        if (after != before - 1) {
            log_execute_stage(2, "material_observation", static_cast<uint32_t>(i),
                              kQolErrConflict, before, after);
            if (!update_in_place) {
                discard_uncommitted_product(product, "material_observation",
                                            static_cast<uint32_t>(i));
            }
            return CraftExecuteResult::kMaterialObservationFailed;
        }
        category_plan.current_count = after;
        log_material_remove(static_cast<uint32_t>(i),
                            used_discard ? "discard"
                                         : (stackable[i] ? "category_fallback" : "remove_item"),
                            before, after);
    }

    // place 自动落位等价于原 INVEN_SaveItem（FindSaveSlot + 同类别堆合并/adopt，H-13）。
    // 成功即把 handle 交给原生库存；失败保持 detached，但此处仍按迁移前的保守策略不再
    // 探测/释放该 handle，ownership 以 action 状态为准。
    if (update_in_place) {
        // 产物就是第 1 格原物品，已在槽内；仅记录提交完成。
        log_execute_stage(0, "product_save_in_place", 0, kQolOk, 0, 1);
    } else {
        qol::contracts::QolItemPlaceTarget place_target{};
        place_target.struct_size = sizeof(qol::contracts::QolItemPlaceTarget);
        place_target.ref = {qol::contracts::kQolBagSpaceOriginal, -1};
        place_target.slot = -1;
        const qol::contracts::QolItemPlaceOutcome placed =
            g_inventory_action->place(product, &place_target);
        const bool product_saved = placed.status == qol::contracts::InventoryActionStatus::kOk;
        log_execute_stage(product_saved ? 0 : 2, "product_save", 0,
                          product_saved ? kQolOk : inventory_action_status_code(placed.status),
                          0, placed.count);
        if (!product_saved) {
            // 自动保存失败时对象仍归调用方；此处不得释放，保持与迁移前一致的失败路径。
            log_message(2, "three slot execute stage=product_save index=0 result=%d "
                           "after=ownership_detached",
                        inventory_action_status_code(placed.status));
            return CraftExecuteResult::kProductSaveFailed;
        }
    }
    if (g_verify_log_budget.fetch_sub(1) > 0) {
        log_message(0, "three slot crafted product=%u consumed=%d",
                    static_cast<uint32_t>(product_category),
                    valid_craft_item(items[1]) + valid_craft_item(items[2]) +
                        (update_in_place ? 0 : valid_craft_item(items[0])));
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
    if (def == nullptr ||
        def->count_rule != ::custom_recipe::CountRule::kQualityAndAttributeTier) {
        return;
    }
    int quality_tier = 0;
    int attribute_tier = 0;
    if (tier_up_cost_inputs_for_item(in->item, &quality_tier, &attribute_tier).ok != kQolOk) {
        return;
    }
    if (def->material_count == 0) return;
    int count = def->material_count;
    if (count > 8) count = 8;
    out->count = count;
    for (int i = 0; i < count; ++i) {
        out->needs[i] = ::custom_recipe::material_count_for(quality_tier, attribute_tier,
                                                            in->character_level);
    }
}

int32_t api_make_item(const result::CraftMakeItemInput* in) {
    note_thread_contract("make_item");
    if (in == nullptr) return 1;
    const ::custom_recipe::Def* def = module_recipe_for_mix_type(in->mix_type);
    if (def == nullptr) return -1;  // 非本层配方：界面转调原版
    // fail-closed：停用期间绝不回落到原版（注入记录号对原表是越界下标）。
    if (!enabled()) return 1;
    if (!valid_craft_item(in->item) || in->rand_inclusive == nullptr) return 1;

    uint16_t category = 0;
    bool is_jewel = false;
    if (query_jewel_fact(in->item, &category, &is_jewel).ok != kQolOk) return 1;

    if (is_jewel) {
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

        if (g_item_payload == nullptr || g_item_payload->update == nullptr ||
            g_inventory_transfer == nullptr || g_inventory_transfer->replace_item == nullptr) {
            log_message(2, "make item aborted: item payload/replace unavailable");
            return 1;
        }
        const int quality_tier =
            ::custom_recipe::jewel_quality_tier_from_category(static_cast<int>(category));
        const int attribute_tier =
            ::custom_recipe::attribute_tier_from_quality_level(current.quality.level);
        MaterialNeedPlan material_plan{};
        const QolResult material_result =
            prepare_material_needs(def, quality_tier, attribute_tier, &material_plan);
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
        MakeItemInputScope input_scope(in);
        const QolResult computed = compute_tier_up_value(
            current_tier, range, &evaluate_candidate_quality, &evaluation,
            &make_item_rand_bridge, &upgrade_result, &target, &new_value);
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
        const contracts::QolItemPayloadMutation mutation{
            static_cast<int32_t>(contracts::QolItemPayloadMutationKind::kJewelWord), 0,
            static_cast<int64_t>(updated_word)};
        contracts::QolItemPayload payload{};
        const QolResult updated = update_item_payload(in->item, &mutation, 1, &payload);
        if (updated.ok != kQolOk) {
            log_message(2, "make item failed stage=payload_update result=%d", updated.code);
            return 1;
        }
        contracts::QolBagMutationResult replaced{};
        const QolResult jewel_written = replace_item_from_payload(in->item, payload, &replaced);
        if (jewel_written.ok != kQolOk) {
            log_message(2,
                        "make item failed stage=jewel_replace result=%d needs=already_written "
                        "category=%u count=%u payload=%u",
                        jewel_written.code, payload.category, payload.count,
                        payload.payload_size);
            return 1;
        }
        if (g_verify_log_budget.fetch_sub(1) > 0) {
            log_message(0, "make item tier-up type=%d value=%d->%d range=[%d,%d] target=%d", type,
                        value, new_value, range.min, range.max, tier_ordinal(target));
        }
        return 0;
    }

    qol::contracts::QolItemType item_type = qol::contracts::QolItemType::kUnknown;
    if (read_item_type(in->item, &item_type).ok != kQolOk ||
        item_type != qol::contracts::QolItemType::kEquipment) {
        return 1;
    }
    const qol::contracts::AttributeQualityApi* api = nullptr;
    if (require_attribute_quality_api(&api).ok != kQolOk) return 1;
    if (g_item_facts->view == nullptr) return 1;
    qol::contracts::QolItemInventoryView view{};
    if (g_item_facts->view(in->item, &view).ok != kQolOk ||
        (view.fields & qol::contracts::kQolItemInventoryHasNativeOptions) == 0 ||
        (view.fields & qol::contracts::kQolItemInventoryOptionsTruncated) != 0 ||
        view.native_option_count == 0 ||
        view.native_option_count > qol::contracts::kQolItemInventoryNativeOptionCapacity) {
        return 1;
    }

    struct AffixCandidate {
        uint32_t native_index;
        int32_t option_id;
        int32_t value;
        int32_t level;
        qol::contracts::AttributeRange range;
    };
    AffixCandidate candidates[qol::contracts::kQolItemInventoryNativeOptionCapacity]{};
    uint32_t candidate_count = 0;
    for (uint32_t i = 0; i < view.native_option_count; ++i) {
        const qol::contracts::QolItemInventoryOptionView& option = view.native_options[i];
        if (option.type != static_cast<int32_t>(
                               qol::contracts::QolItemInventoryOptionType::kNative)) {
            continue;
        }
        qol::contracts::AttributeQualityItem context{};
        const QolResult queried = api->query_option_context(
            in->item, option.id, option.value, nullptr, &context);
        if (queried.ok != kQolOk ||
            !::custom_recipe::affix_upgradable(context.status, context.quality.level,
                                               context.range)) {
            continue;
        }
        candidates[candidate_count++] =
            AffixCandidate{i, option.id, option.value, context.quality.level, context.range};
    }
    if (candidate_count == 0) return 1;

    MakeItemInputScope input_scope(in);
    int32_t pick = -1;
    const QolResult picked = in->rand_inclusive(0, static_cast<int32_t>(candidate_count - 1), &pick);
    if (picked.ok != kQolOk || pick < 0 || static_cast<uint32_t>(pick) >= candidate_count) {
        log_message(2, "make item aborted: equipment affix random failed code=%d pick=%d count=%u",
                    picked.ok == kQolOk ? kQolErrInternal : picked.code, pick, candidate_count);
        return 1;
    }
    const AffixCandidate& candidate = candidates[static_cast<uint32_t>(pick)];
    QualityEvaluationContext evaluation{api};
    ::custom_recipe::Tier target = ::custom_recipe::Tier::Grey;
    int32_t new_value = 0;
    UpgradeResult upgrade_result = UpgradeResult::kDegenerate;
    const QolResult computed = compute_tier_up_value(
        static_cast<::custom_recipe::Tier>(candidate.level), candidate.range,
        &evaluate_candidate_quality, &evaluation, &make_item_rand_bridge,
        &upgrade_result, &target, &new_value);
    if (computed.ok != kQolOk || upgrade_result != UpgradeResult::kOk) {
        log_message(2, "make item aborted: equipment tier evaluation failed code=%d result=%d",
                    computed.code, static_cast<int32_t>(upgrade_result));
        return 1;
    }
    if (new_value < std::numeric_limits<int16_t>::min() ||
        new_value > std::numeric_limits<int16_t>::max()) {
        log_message(3, "make item aborted: equipment value out of int16 range value=%d", new_value);
        return 1;
    }
    if ((view.fields & qol::contracts::kQolItemInventoryHasRarity) == 0) return 1;
    const int quality_tier = ::custom_recipe::equipment_quality_tier_from_rarity(view.rarity);
    // 消耗输入的属性等阶取全部候选（可升阶词条）的最低 level：与面板显示共用同一语义。
    int min_level = candidates[0].level;
    for (uint32_t i = 1; i < candidate_count; ++i) {
        if (candidates[i].level < min_level) min_level = candidates[i].level;
    }
    const int attribute_tier = ::custom_recipe::attribute_tier_from_quality_level(min_level);
    if (quality_tier < 0 || attribute_tier < 0) return 1;
    if (g_item_payload == nullptr || g_item_payload->update == nullptr ||
        g_inventory_transfer == nullptr || g_inventory_transfer->replace_item == nullptr) {
        log_message(2, "make item aborted: item payload/replace unavailable");
        return 1;
    }
    MaterialNeedPlan material_plan{};
    const QolResult material_result =
        prepare_material_needs(def, quality_tier, attribute_tier, &material_plan);
    if (material_result.ok != kQolOk) {
        log_message(2, "make item aborted: material precheck failed code=%d", material_result.code);
        return 1;
    }

    const QolResult needs_written = write_material_needs(def, material_plan);
    if (needs_written.ok != kQolOk) return 1;

    const contracts::QolItemPayloadMutation mutation{
        static_cast<int32_t>(contracts::QolItemPayloadMutationKind::kNativeOptionValue),
        candidate.native_index, static_cast<int64_t>(new_value)};
    contracts::QolItemPayload payload{};
    const QolResult updated = update_item_payload(in->item, &mutation, 1, &payload);
    if (updated.ok != kQolOk) {
        log_message(2, "make item failed stage=payload_update result=%d", updated.code);
        return 1;
    }
    contracts::QolBagMutationResult replaced{};
    const QolResult option_written = replace_item_from_payload(in->item, payload, &replaced);
    if (option_written.ok != kQolOk) {
        log_message(2,
                    "make item failed stage=native_option_replace result=%d needs=already_written "
                    "category=%u count=%u payload=%u",
                    option_written.code, payload.category, payload.count, payload.payload_size);
        return 1;
    }
    if (g_verify_log_budget.fetch_sub(1) > 0) {
        log_message(0,
                    "make item equipment tier-up option_index=%u option_id=%d value=%d->%d "
                    "range=[%d,%d] target=%d",
                    candidate.native_index, candidate.option_id, candidate.value, new_value,
                    candidate.range.min, candidate.range.max, tier_ordinal(target));
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
    &api_is_tier_up_recipe,
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
    // 表注入只有一个入口：宿主资源的 ownership acquire。typed 结果保留错误域
    // （NotReady/Busy 可重试；Unsupported 永久；Internal 内部错误），不再折叠为 bool。
    const QolResult result = ::custom_recipe::custom_recipe_table_acquire();
    if (result.ok == kQolOk) {
        g_ownership_ready = true;
        return 0;
    }
    // 失败即 total 清理（收回记录数、清模块状态与快照），并按**原码**返回，便于宿主区分
    // 是否可以定向重试（NotReady）或应视为永久错误（Unsupported/Internal）。
    ::custom_recipe::custom_recipe_table_deactivate();
    log_message(2, "custom_recipe table acquire failed code=%d",
                result.code != 0 ? result.code : result.ok);
    return result.code != 0 ? result.code : result.ok;
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
    if (g_save_ready_subscription != 0 || g_sdk == nullptr) return;
    g_event_state = contracts::event_state_if_available(g_sdk);
    if (g_event_state == nullptr) return;
    (void)g_event_state->subscribe(contracts::kEventStateSaveReady,
                                   &refresh_dynamic_recipes_event, nullptr,
                                   &g_save_ready_subscription);
}

void unregister_dynamic_recipes() {
    if (g_save_ready_subscription != 0 && g_event_state != nullptr) {
        (void)g_event_state->unsubscribe(g_save_ready_subscription);
    }
    g_save_ready_subscription = 0;
    g_event_state = nullptr;
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
