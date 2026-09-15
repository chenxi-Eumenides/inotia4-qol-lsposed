#include "custom_recipe_catalog.h"

namespace custom_recipe {

namespace {

// 配方 1 材料：卓越灵药（ITEMDATABASE 记录下标 15，可消耗类），数量 1。
constexpr uint16_t kElixirOfExcellenceItemId = 15;
// 配方按钮文本默认 wordId：35291「宝石强化」（复用既有本地化串，见 §7.2）。
constexpr uint16_t kDefaultLabelWordId = 35291;

const Material kRecipe1Materials[] = {
    {kElixirOfExcellenceItemId, 1},
};

// 静态目录（唯一真源）：新增配方在此追加一项即可。
// 材料需求数量 = ceil(1 × 宝石档位 × (105 − 角色等级) / 105)（见 CountRule::kJewelGradeAndLevel）：
// 低级宝石 1 瓶、中级 2 瓶…混沌宝石 5 瓶，随角色等级线性递减，105 级为 0 瓶。
const Def kCatalog[] = {
    {kDefaultLabelWordId, Kind::kJewelTierUp, kRecipe1Materials,
     static_cast<uint8_t>(sizeof(kRecipe1Materials) / sizeof(kRecipe1Materials[0])),
     /*cost_word_id=*/0, CountRule::kJewelGradeAndLevel},
};

constexpr size_t kCatalogLen = sizeof(kCatalog) / sizeof(kCatalog[0]);

uint16_t g_base_record_count = 0;
bool g_bound = false;

}  // namespace

const Def* catalog(size_t* out_count) {
    if (out_count != nullptr) *out_count = kCatalogLen;
    return kCatalog;
}

bool catalog_ready() { return g_bound; }

void bind_base_record_count(uint16_t base_record_count) {
    g_base_record_count = base_record_count;
    g_bound = true;
}

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
