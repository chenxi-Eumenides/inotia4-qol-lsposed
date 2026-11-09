#include "catalog.h"

#include <cstring>
#include <new>
#include <utility>

#include "rules.h"

namespace custom_recipe {

namespace {

// 配方材料：卓越灵药（ITEMDATABASE 记录下标 15，可消耗类），数量 1。
constexpr uint16_t kElixirOfExcellenceItemId = 15;
// 「合成」入口（Kind::kThreeSlotCraft）材料：低级宝石（类别 28）×3，镜像原版 record 12 的
// 材料条目，仅用于该页材料格/费用**显示**保持原样；能否合成与实际产物由 `kThreeSlotRecipes`
// 按 3 格内容查表决定（§2.8/§4.12），不走原版 3:1 路径。
constexpr uint16_t kLowJewelItemId = 28;
constexpr uint8_t kThreeSlotMaterialCount = 3;
// 配方按钮文本 wordId（复用既有本地化串，见 §7.2）：35216「合成」。
// 「宝石强化」条目复用 35291，但按钮显示由 MEMORYTEXT_GetText 替换为模块字面量「升阶」。
constexpr uint16_t kNativeLabelWordId = 35216;
// 「合成」入口费用公式 wordId：镜像原版 record 12（费用显示走原版公式，不由模块强制 0）。
constexpr uint16_t kNativeCostWordId = 188;
// 「宝石强化」条目的费用公式 wordId：**必须填合法公式**，不能填 0（免费配方的取值）。
// 原因（真机崩溃 + 反汇编定案）：`UIMix_ButtonRecipeExe` 内部会自己调 `UIMix_InitMixingState`，
// 而此时页面的 type 仍是宝石强化页的 1（模块的换型发生在转调之后）。type==1 分支
// （`InitMixingState` 0xc00f0 `cmp x0,#1; b.eq 0xc01f0`）会读本记录 `b8-9` 作公式：
// `0xc0224 bl MEMORYTEXT_GetText_E(b8-9)` → `0xc0230 bl CAL_Calculate(公式)`。
// 本记录 `b8-9 = 0` 时 `GetText_E(0)` 返回空 → `CAL_Calculate(NULL)` 空指针解引用崩溃。
// 填 188（与原版 record 12 同一公式）即与原版行为完全一致；实际费用仍由放料 hook 强制 0。
constexpr uint16_t kTierUpCostWordId = 188;
// 挂载页签：宝石强化页（type 1）= 配方组 group 3（注入记录 b11 = 1<<3 = 0x08）。
constexpr uint8_t kPageGroupJewelCraft = 3;

const Material kNativeMaterials[] = {
    {kLowJewelItemId, kThreeSlotMaterialCount},
};

const Material kTierUpMaterials[] = {
    {kElixirOfExcellenceItemId, 1},
};

// 静态目录（唯一真源）：新增配方在此追加一项即可。目录顺序 = 配方按钮顺序。
// 两条均挂宝石强化页（group 3）；注入时原版 12..15 记录的组位会被清掉（custom_recipe_table），
// 该页配方列表 = [69「合成」, 70「宝石强化」]。
// 第 1 条「合成」= kThreeSlotCraft：3 格隐式配方（§2.8/§4.12）。放料与合成由模块自实现
// （放料不转调原函数、合成前置查表 + 原生 YesNo 回调）；产物 = `three_slot_recipes` 命中项的
// `product`。材料显示仍镜像原版 record 12（`{28,3}` / 费用文本 188）以保持该页格位与费用显示不变。
// 第 2 条「升阶」= kTierUp：借 type 3 形态，材料需求数量
// = max(0, 品质档位 + 属性等阶 − F)，F = floor((等级 − 25) × 8 / 70)
// （见 CountRule::kQualityAndAttributeTier 与 rules.h 的 material_count_for）；
// 品质档位：宝石按类别（28..32 → 0..4）、装备按稀有度（0..4 → 0..4）；
// 属性等阶：宝石取 quality.level、装备取可升阶词条最低 level；
// 25 级起最低组合 0 瓶、95 级起全部组合 0 瓶；费用 0 由 hook 强制。
const Def kCatalog[] = {
    {kNativeLabelWordId, Kind::kThreeSlotCraft, kNativeMaterials,
     static_cast<uint8_t>(sizeof(kNativeMaterials) / sizeof(kNativeMaterials[0])),
     /*cost_word_id=*/kNativeCostWordId, CountRule::kFixed, Form::kMultiInputCreate,
     kPageGroupJewelCraft},
    {kTierUpLabelWordId, Kind::kTierUp, kTierUpMaterials,
     static_cast<uint8_t>(sizeof(kTierUpMaterials) / sizeof(kTierUpMaterials[0])),
     /*cost_word_id=*/kTierUpCostWordId, CountRule::kQualityAndAttributeTier,
     Form::kTargetAndCreate,
     kPageGroupJewelCraft},
};

constexpr size_t kCatalogLen = sizeof(kCatalog) / sizeof(kCatalog[0]);

// ---- 3 格隐式配方表（§2.8 冻结，唯一真源；表序即匹配优先级）----
// 物品类别（== itemId）：4=背包（大）、5=恢复药水（小）、16=低级武器强化卷轴、
// 28..32=低/中/高/顶级宝石 + 混沌宝石、35=皮革、41=魔法衣料。
constexpr uint16_t kCategoryBackpackLarge = 4;
constexpr uint16_t kCategoryMinorHealingPotion = 5;
constexpr uint16_t kCategoryLowWeaponScroll = 16;
// 混沌卷轴（ITEMDATABASE 实证：类别 20 → text_id 50「混沌武器强化卷轴」；类别 25 → 55「混沌防具强化卷轴」；
// 全表满足「类别 = 名称 text_id − 30」，与 16→46「低级武器强化卷轴」、28→58「低级宝石」一致）。
constexpr uint16_t kCategoryChaosWeaponScroll = 20;
constexpr uint16_t kCategoryChaosArmorScroll = 25;
constexpr uint16_t kCategoryLowJewel = 28;
constexpr uint16_t kCategoryMidJewel = 29;
constexpr uint16_t kCategoryHighJewel = 30;
constexpr uint16_t kCategoryTopJewel = 31;
constexpr uint16_t kCategoryChaosJewel = 32;
constexpr uint16_t kCategoryLeather = 35;
constexpr uint16_t kCategoryMagicCloth = 41;
constexpr uint16_t kCategoryVitalityPotion = 14;  // 元气恢复药水（ITEMDATABASE 记录下标）

const ThreeSlotRecipe kThreeSlotRecipes[] = {
    // 皮革 + 空槽 + 魔法衣料 → 背包（大）：槽位严格匹配（第 2 格必须空）。
    {{kCategoryLeather, 0, kCategoryMagicCloth}, true, kCategoryBackpackLarge,
     ProductMode::kFixedCategory, 0, false, ProductOrigin::kNewItem},
    // 恢复药水（小）×2 + 低级武器强化卷轴 → 顶级宝石：顺序无关。
    {{kCategoryMinorHealingPotion, kCategoryMinorHealingPotion, kCategoryLowWeaponScroll}, false,
     kCategoryTopJewel, ProductMode::kFixedCategory, 0, false, ProductOrigin::kNewItem},
    // 3 颗同级宝石 → 高一级（含顶级 → 混沌）。
    {{kCategoryLowJewel, kCategoryLowJewel, kCategoryLowJewel}, false, kCategoryMidJewel,
     ProductMode::kFixedCategory, 0, false, ProductOrigin::kNewItem},
    {{kCategoryMidJewel, kCategoryMidJewel, kCategoryMidJewel}, false, kCategoryHighJewel,
     ProductMode::kFixedCategory, 0, false, ProductOrigin::kNewItem},
    {{kCategoryHighJewel, kCategoryHighJewel, kCategoryHighJewel}, false, kCategoryTopJewel,
     ProductMode::kFixedCategory, 0, false, ProductOrigin::kNewItem},
    {{kCategoryTopJewel, kCategoryTopJewel, kCategoryTopJewel}, false, kCategoryChaosJewel,
     ProductMode::kFixedCategory, 0, false, ProductOrigin::kNewItem},
    // 宝石 + 混沌武器强化卷轴(20) + 混沌防具强化卷轴(25) → **该宝石自身**，数值 ×1.2 向上取整（槽位严格顺序）。
    // 第 1 格接受任意宝石（28..32），产物沿用源宝石的类别、随机等级与属性类型，只缩放数值位。
    {{kAnyJewelSlot, kCategoryChaosWeaponScroll, kCategoryChaosArmorScroll}, true, 0,
     ProductMode::kScaleFirstItem, 1200, false, ProductOrigin::kUpdateExistingItem, 0},
    // 两件**相同**的特殊装备（槽 0/2，各一件）+ 元气恢复药水(14) → 同类别特殊装备新建，
    // 宝石孔总数与剩余强化次数拉满（槽位严格顺序；两件装备必须同类别，见 same_first_last）。
    // 「特殊装备」= ITEMCLASSBASE 记录 +7 bit4 置位的那 26 条，判定由调用方注入谓词。
    {{kAnySpecialEquipSlot, kCategoryVitalityPotion, kAnySpecialEquipSlot}, true, 0,
     ProductMode::kMaxSocketEnchantFirstItem, 0, true, ProductOrigin::kNewItem},
};

constexpr size_t kThreeSlotRecipeLen = sizeof(kThreeSlotRecipes) / sizeof(kThreeSlotRecipes[0]);
constexpr size_t kThreeSlotCount = sizeof(kThreeSlotRecipes[0].slots) /
                                   sizeof(kThreeSlotRecipes[0].slots[0]);

// 3 元素升序（值小→大）就地排序；用于多重集（顺序无关）比较。
void sort_three(uint16_t* v) {
    for (size_t i = 1; i < kThreeSlotCount; ++i) {
        const uint16_t key = v[i];
        size_t j = i;
        while (j > 0 && v[j - 1] > key) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = key;
    }
}

uint16_t g_base_record_count = 0;
bool g_bound = false;

// 动态配方表（进档时由 set_dynamic_three_slot_recipes 重建；只在游戏主线程读写）。
ThreeSlotRecipe g_dynamic_recipes[kMaxDynamicRecipes];
size_t g_dynamic_recipe_count = 0;
uint64_t g_dynamic_recipe_generation = 0;

void advance_dynamic_recipe_generation() {
    ++g_dynamic_recipe_generation;
    if (g_dynamic_recipe_generation == 0) ++g_dynamic_recipe_generation;
}

QolResult catalog_error(int32_t code, const char* message) {
    return qol::contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

}  // namespace

const Def* catalog(size_t* out_count) {
    if (out_count != nullptr) *out_count = kCatalogLen;
    return kCatalog;
}

const ThreeSlotRecipe* three_slot_recipes(size_t* out_count) {
    if (out_count != nullptr) *out_count = kThreeSlotRecipeLen;
    return kThreeSlotRecipes;
}

namespace {

// 在给定配方表里按 3 格类别匹配（静态表与动态表共用同一套匹配语义）。
const ThreeSlotRecipe* match_in_table(const ThreeSlotRecipe* table, size_t count,
                                      const uint16_t slots[3],
                                      bool (*is_special_equip)(uint16_t category),
                                      const bool jewel_slots[3]) {
    uint16_t sorted_actual[kThreeSlotCount] = {slots[0], slots[1], slots[2]};
    sort_three(sorted_actual);
    for (size_t i = 0; i < count; ++i) {
        const ThreeSlotRecipe& recipe = table[i];
        if (recipe.ordered) {
            // 「两件相同的 X」：槽 0 与槽 2 必须同类别（先判，避免通配符把它们各自放宽）。
            if (recipe.same_first_last && slots[0] != slots[2]) continue;
            bool hit = true;
            for (size_t s = 0; s < kThreeSlotCount; ++s) {
                const uint16_t want = recipe.slots[s];
                if (want == kAnyJewelSlot) {
                    // 通配：只接受调用方按 item handle/item_type 判定的宝石。
                    if (jewel_slots == nullptr || !jewel_slots[s]) {
                        hit = false;
                        break;
                    }
                    continue;
                }
                if (want == kAnySpecialEquipSlot) {
                    // 通配：该格接受任意特殊装备（+7 bit4）。谓词缺省 → 该配方不命中（fail-closed）。
                    if (is_special_equip == nullptr || !is_special_equip(slots[s])) {
                        hit = false;
                        break;
                    }
                    continue;
                }
                if (slots[s] != want) {
                    hit = false;
                    break;
                }
            }
            if (hit) return &recipe;
            continue;
        }
        uint16_t sorted_recipe[kThreeSlotCount] = {recipe.slots[0], recipe.slots[1],
                                                   recipe.slots[2]};
        sort_three(sorted_recipe);
        bool hit = true;
        for (size_t s = 0; s < kThreeSlotCount; ++s) {
            if (sorted_actual[s] != sorted_recipe[s]) {
                hit = false;
                break;
            }
        }
        if (hit) return &recipe;
    }
    return nullptr;
}

bool catalog_is_special_equip(uint16_t category) {
    for (size_t i = 0; i < kSpecialEquipCategoryCount; ++i) {
        if (kSpecialEquipCategories[i] == category) return true;
    }
    return false;
}

uint64_t splitmix64_next(uint64_t* state) {
    *state += UINT64_C(0x9E3779B97F4A7C15);
    uint64_t value = *state;
    value = (value ^ (value >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31);
}

constexpr size_t kMaximumDynamicMaterialPool = kMaxDynamicRecipes;
constexpr size_t kMaximumCandidateAttempts = 4096;

void make_seeded_candidate(uint64_t* state, uint16_t product,
                           const uint16_t* material_pool, size_t pool_size,
                           ThreeSlotRecipe* out) {
    size_t candidate_indices[kMaximumDynamicMaterialPool + 1]{};
    const size_t candidate_count = pool_size + 1;  // index pool_size represents the empty slot.
    for (size_t i = 0; i < candidate_count; ++i) candidate_indices[i] = i;
    for (size_t slot = 0; slot < kThreeSlotCount; ++slot) {
        const size_t remaining = candidate_count - slot;
        const size_t selected =
            slot + static_cast<size_t>(splitmix64_next(state) % static_cast<uint64_t>(remaining));
        const size_t swapped = candidate_indices[slot];
        candidate_indices[slot] = candidate_indices[selected];
        candidate_indices[selected] = swapped;
        const size_t picked = candidate_indices[slot];
        out->slots[slot] = picked == pool_size ? 0 : material_pool[picked];
    }
    out->ordered = true;
    out->product = product;
    out->product_mode = ProductMode::kFixedCategory;
    out->scale_permille = 0;
    out->same_first_last = false;
    out->product_origin = ProductOrigin::kNewItem;
}

}  // namespace

bool three_slot_recipe_matches(const ThreeSlotRecipe& recipe, const uint16_t slots[3],
                               bool (*is_special_equip)(uint16_t category),
                               const bool jewel_slots[3]) {
    return slots != nullptr &&
           match_in_table(&recipe, 1, slots, is_special_equip, jewel_slots) != nullptr;
}

const ThreeSlotRecipe* match_static_three_slot(
    const uint16_t slots[3], bool (*is_special_equip)(uint16_t category),
    const bool jewel_slots[3]) {
    if (slots == nullptr) return nullptr;
    return match_in_table(kThreeSlotRecipes, kThreeSlotRecipeLen, slots, is_special_equip,
                          jewel_slots);
}

QolResult build_dynamic_recipes_seeded(const uint16_t* equip_categories, size_t equip_count,
                                       uint64_t seed, const uint16_t* material_pool,
                                       size_t pool_size, ThreeSlotRecipe* out,
                                       size_t out_capacity, size_t* out_count,
                                       size_t* out_reroll_count) {
    if (out_count == nullptr) {
        return catalog_error(kQolErrInvalidArgument, "null dynamic recipe count output");
    }
    *out_count = 0;
    if (out_reroll_count != nullptr) *out_reroll_count = 0;
    if ((equip_count > 0 && equip_categories == nullptr) || material_pool == nullptr ||
        (equip_count > 0 && out == nullptr) || seed == 0 || pool_size < kThreeSlotCount ||
        pool_size > kMaximumDynamicMaterialPool || equip_count > kMaxDynamicRecipes ||
        out_capacity < equip_count) {
        return catalog_error(kQolErrInvalidArgument, "invalid dynamic recipe input");
    }

    for (size_t i = 0; i < pool_size; ++i) {
        if (material_pool[i] == 0) {
            return catalog_error(kQolErrInvalidArgument,
                                 "dynamic material pool contains empty id");
        }
        for (size_t j = 0; j < i; ++j) {
            if (material_pool[i] == material_pool[j]) {
                return catalog_error(kQolErrInvalidArgument,
                                     "dynamic material pool contains duplicates");
            }
        }
    }

    size_t written = 0;
    size_t rerolls = 0;
    for (size_t e = 0; e < equip_count; ++e) {
        if (equip_categories[e] == 0) {
            return catalog_error(kQolErrInvalidArgument, "dynamic equipment category is empty");
        }
        uint64_t state = seed ^
                         (static_cast<uint64_t>(equip_categories[e]) *
                          UINT64_C(0xD6E8FEB86659FD93));
        ThreeSlotRecipe candidate{};
        bool accepted = false;
        for (size_t attempt = 0; attempt < kMaximumCandidateAttempts; ++attempt) {
            make_seeded_candidate(&state, equip_categories[e], material_pool, pool_size,
                                  &candidate);
            bool conflict =
                match_static_three_slot(candidate.slots, &catalog_is_special_equip) != nullptr;
            for (size_t previous = 0; !conflict && previous < written; ++previous) {
                conflict = three_slot_recipe_matches(out[previous], candidate.slots,
                                                      &catalog_is_special_equip);
            }
            if (!conflict) {
                accepted = true;
                break;
            }
            ++rerolls;
        }
        if (!accepted) {
            if (out_reroll_count != nullptr) *out_reroll_count = rerolls;
            return catalog_error(kQolErrInternal,
                                 "no non-conflicting dynamic recipe candidate found");
        }
        out[written] = candidate;
        ++written;
    }
    *out_count = written;
    if (out_reroll_count != nullptr) *out_reroll_count = rerolls;
    return qol::contracts::ok();
}

QolResult dynamic_recipe_cache_get_or_build(DynamicRecipeCache* cache,
                                            uint64_t creation_time_ms,
                                            DynamicRecipeGeneratorFn generator, void* context,
                                            ThreeSlotRecipe* out, size_t out_capacity,
                                            size_t* out_count, bool* out_cache_hit) {
    if (out_count != nullptr) *out_count = 0;
    if (out_cache_hit != nullptr) *out_cache_hit = false;
    if (cache == nullptr || creation_time_ms == 0 || generator == nullptr || out == nullptr ||
        out_capacity == 0) {
        return catalog_error(kQolErrInvalidArgument, "invalid dynamic recipe cache input");
    }

    const auto cached = cache->entries.find(creation_time_ms);
    if (cached != cache->entries.end()) {
        const std::vector<ThreeSlotRecipe>& recipes = cached->second;
        if (recipes.empty() || recipes.size() > kMaxDynamicRecipes) {
            return catalog_error(kQolErrInternal, "dynamic recipe cache entry is invalid");
        }
        if (recipes.size() > out_capacity) {
            return catalog_error(kQolErrInvalidArgument,
                                 "dynamic recipe cache output capacity too small");
        }
        for (size_t i = 0; i < recipes.size(); ++i) out[i] = recipes[i];
        if (out_count != nullptr) *out_count = recipes.size();
        if (out_cache_hit != nullptr) *out_cache_hit = true;
        return qol::contracts::ok();
    }

    ThreeSlotRecipe generated[kMaxDynamicRecipes]{};
    size_t generated_count = 0;
    const QolResult result = generator(creation_time_ms, generated, kMaxDynamicRecipes,
                                       &generated_count, context);
    if (result.ok != kQolOk) return result;
    if (generated_count == 0 || generated_count > kMaxDynamicRecipes) {
        return catalog_error(kQolErrInternal, "dynamic recipe generator returned invalid count");
    }
    if (generated_count > out_capacity) {
        return catalog_error(kQolErrInvalidArgument,
                             "dynamic recipe cache output capacity too small");
    }

    try {
        std::vector<ThreeSlotRecipe> recipes(generated, generated + generated_count);
        const auto inserted = cache->entries.emplace(creation_time_ms, std::move(recipes));
        if (!inserted.second) {
            return catalog_error(kQolErrConflict,
                                 "dynamic recipe cache key appeared during generation");
        }
    } catch (const std::bad_alloc&) {
        return catalog_error(kQolErrInternal, "dynamic recipe cache allocation failed");
    }

    for (size_t i = 0; i < generated_count; ++i) out[i] = generated[i];
    if (out_count != nullptr) *out_count = generated_count;
    return qol::contracts::ok();
}

void set_dynamic_three_slot_recipes(const ThreeSlotRecipe* recipes, size_t count) {
    if (recipes == nullptr || count == 0) {
        g_dynamic_recipe_count = 0;
        advance_dynamic_recipe_generation();
        return;
    }
    if (count > kMaxDynamicRecipes) count = kMaxDynamicRecipes;
    for (size_t i = 0; i < count; ++i) g_dynamic_recipes[i] = recipes[i];
    g_dynamic_recipe_count = count;
    advance_dynamic_recipe_generation();
}

size_t dynamic_three_slot_recipe_count() { return g_dynamic_recipe_count; }

uint64_t dynamic_recipe_generation() { return g_dynamic_recipe_generation; }

const ThreeSlotRecipe* match_three_slot(const uint16_t slots[3],
                                        bool (*is_special_equip)(uint16_t category),
                                        const bool jewel_slots[3]) {
    if (slots == nullptr) return nullptr;
    // 静态表优先（固定配方语义不受动态表影响），未命中再查进档随机生成的动态表。
    const ThreeSlotRecipe* hit =
        match_static_three_slot(slots, is_special_equip, jewel_slots);
    if (hit != nullptr) return hit;
    return match_in_table(g_dynamic_recipes, g_dynamic_recipe_count, slots, is_special_equip,
                          jewel_slots);
}

uint32_t delegate_mix_type_for(const ThreeSlotRecipe& recipe, const uint16_t slots[3]) {
    if (slots == nullptr) return 0;
    const uint16_t src = slots[0];
    // 档位宝石 28..31（28 低/29 中/30 高/31 顶级）三同 → 原版宝石配方 12..15。
    if (src < 28 || src > 31) return 0;
    if (slots[1] != src || slots[2] != src) return 0;
    if (recipe.product != static_cast<uint16_t>(src + 1)) return 0;
    return static_cast<uint32_t>(recipe.product) - 17u;
}

bool catalog_ready() { return g_bound; }

void bind_base_record_count(uint16_t base_record_count) {
    g_base_record_count = base_record_count;
    g_bound = true;
}

uint16_t bound_base_record_count() { return g_bound ? g_base_record_count : 0; }

const Def* def_for_mix_type(uint32_t mix_type) {
    if (!g_bound) return nullptr;
    if (mix_type < static_cast<uint32_t>(g_base_record_count)) return nullptr;
    const size_t index = static_cast<size_t>(mix_type - g_base_record_count);
    if (index >= kCatalogLen) return nullptr;
    return &kCatalog[index];
}

uint32_t mix_type_at(uint16_t base_record_count, size_t index) {
    return static_cast<uint32_t>(base_record_count) + static_cast<uint32_t>(index);
}

uint32_t material_start_at(uint16_t base_material_count, const Def* cat, size_t cat_len,
                           size_t index) {
    uint32_t start = base_material_count;
    const size_t limit = (index < cat_len) ? index : cat_len;
    for (size_t i = 0; i < limit; ++i) {
        start += cat[i].material_count;
    }
    return start;
}

uint32_t material_total(const Def* cat, size_t cat_len) {
    uint32_t total = 0;
    for (size_t i = 0; i < cat_len; ++i) {
        total += cat[i].material_count;
    }
    return total;
}

}  // namespace custom_recipe
