#include "custom_recipe_catalog.h"

#include "custom_recipe_rules.h"

namespace custom_recipe {

namespace {

// 配方材料：卓越灵药（ITEMDATABASE 记录下标 15，可消耗类），数量 1。
constexpr uint16_t kElixirOfExcellenceItemId = 15;
// 「合成」入口（Kind::kThreeSlotCraft）材料：低级宝石（类别 28）×3，镜像原版 record 12 的
// 材料条目，仅用于该页材料格/费用**显示**保持原样；能否合成与实际产物由 `kThreeSlotRecipes`
// 按 3 格内容查表决定（§2.8/§4.12），不走原版 3:1 路径。
constexpr uint16_t kLowJewelItemId = 28;
constexpr uint8_t kGemCraftStuffCount = 3;
// 配方按钮文本 wordId（复用既有本地化串，见 §7.2）：35216「合成」。
// 「宝石强化」条目复用 35291，但按钮显示由 MEMORYTEXT_GetText 替换为模块字面量
// 「宝石升阶」（custom_recipe_catalog.h 的 kJewelTierUpLabel）。
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
constexpr uint16_t kJewelTierUpCostWordId = 188;
// 挂载页签：宝石强化页（type 1）= 配方组 group 3（注入记录 b11 = 1<<3 = 0x08）。
constexpr uint8_t kPageGroupJewelCraft = 3;

const Material kNativeMaterials[] = {
    {kLowJewelItemId, kGemCraftStuffCount},
};

const Material kJewelTierUpMaterials[] = {
    {kElixirOfExcellenceItemId, 1},
};

// 静态目录（唯一真源）：新增配方在此追加一项即可。目录顺序 = 配方按钮顺序。
// 两条均挂宝石强化页（group 3）；注入时原版 12..15 记录的组位会被清掉（custom_recipe_table），
// 该页配方列表 = [69「合成」, 70「宝石强化」]。
// 第 1 条「合成」= kThreeSlotCraft：3 格隐式配方（§2.8/§4.12）。放料与合成由模块自实现
// （放料不转调原函数、合成前置查表 + 原生 YesNo 回调）；产物 = `three_slot_recipes` 命中项的
// `product`。材料显示仍镜像原版 record 12（`{28,3}` / 费用文本 188）以保持该页格位与费用显示不变。
// 第 2 条「宝石强化」= kJewelTierUp：借 type 3 形态，材料需求数量
// = ceil(1 × 宝石档位 × (105 − 角色等级) / 105)（见 CountRule::kJewelGradeAndLevel）：
// 低级宝石 1 瓶、中级 2 瓶…混沌宝石 5 瓶，105 级为 0 瓶；费用 0 由 hook 强制。
const Def kCatalog[] = {
    {kNativeLabelWordId, Kind::kThreeSlotCraft, kNativeMaterials,
     static_cast<uint8_t>(sizeof(kNativeMaterials) / sizeof(kNativeMaterials[0])),
     /*cost_word_id=*/kNativeCostWordId, CountRule::kFixed, Form::kMultiInputCreate,
     kPageGroupJewelCraft},
    {kJewelTierUpLabelWordId, Kind::kJewelTierUp, kJewelTierUpMaterials,
     static_cast<uint8_t>(sizeof(kJewelTierUpMaterials) / sizeof(kJewelTierUpMaterials[0])),
     /*cost_word_id=*/kJewelTierUpCostWordId, CountRule::kJewelGradeAndLevel, Form::kTargetAndCreate,
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

const ThreeSlotRecipe kThreeSlotRecipes[] = {
    // 皮革 + 空槽 + 魔法衣料 → 背包（大）：槽位严格匹配（第 2 格必须空）。
    {{kCategoryLeather, 0, kCategoryMagicCloth}, true, kCategoryBackpackLarge,
     ProductMode::kFixedCategory, 0},
    // 恢复药水（小）×2 + 低级武器强化卷轴 → 顶级宝石：顺序无关。
    {{kCategoryMinorHealingPotion, kCategoryMinorHealingPotion, kCategoryLowWeaponScroll}, false,
     kCategoryTopJewel, ProductMode::kFixedCategory, 0},
    // 3 颗同级宝石 → 高一级（含顶级 → 混沌）。
    {{kCategoryLowJewel, kCategoryLowJewel, kCategoryLowJewel}, false, kCategoryMidJewel,
     ProductMode::kFixedCategory, 0},
    {{kCategoryMidJewel, kCategoryMidJewel, kCategoryMidJewel}, false, kCategoryHighJewel,
     ProductMode::kFixedCategory, 0},
    {{kCategoryHighJewel, kCategoryHighJewel, kCategoryHighJewel}, false, kCategoryTopJewel,
     ProductMode::kFixedCategory, 0},
    {{kCategoryTopJewel, kCategoryTopJewel, kCategoryTopJewel}, false, kCategoryChaosJewel,
     ProductMode::kFixedCategory, 0},
    // 宝石 + 混沌武器强化卷轴(20) + 混沌防具强化卷轴(25) → **该宝石自身**，数值 ×1.2 向上取整（槽位严格顺序）。
    // 第 1 格接受任意宝石（28..32），产物沿用源宝石的类别、随机等级与属性类型，只缩放数值位。
    {{kAnyJewelSlot, kCategoryChaosWeaponScroll, kCategoryChaosArmorScroll}, true, 0,
     ProductMode::kScaleFirstItem, 1200},
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

// 通配槽判定：类别是否落在宝石区间（28..32）。复用规则层的档位函数（非宝石返回 0）。
bool is_jewel_category(uint16_t category) {
    return jewel_grade_from_category(static_cast<int>(category)) > 0;
}

uint16_t g_base_record_count = 0;
bool g_bound = false;

}  // namespace

const Def* catalog(size_t* out_count) {
    if (out_count != nullptr) *out_count = kCatalogLen;
    return kCatalog;
}

const ThreeSlotRecipe* three_slot_recipes(size_t* out_count) {
    if (out_count != nullptr) *out_count = kThreeSlotRecipeLen;
    return kThreeSlotRecipes;
}

const ThreeSlotRecipe* match_three_slot(const uint16_t slots[3]) {
    if (slots == nullptr) return nullptr;
    uint16_t sorted_actual[kThreeSlotCount] = {slots[0], slots[1], slots[2]};
    sort_three(sorted_actual);
    for (size_t i = 0; i < kThreeSlotRecipeLen; ++i) {
        const ThreeSlotRecipe& recipe = kThreeSlotRecipes[i];
        if (recipe.ordered) {
            bool hit = true;
            for (size_t s = 0; s < kThreeSlotCount; ++s) {
                const uint16_t want = recipe.slots[s];
                if (want == kAnyJewelSlot) {
                    // 通配：该格接受任意宝石类别（28..32）。
                    if (!is_jewel_category(slots[s])) {
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
