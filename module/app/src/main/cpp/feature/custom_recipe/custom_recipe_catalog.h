#pragma once

#include <cstddef>
#include <cstdint>

// 自定义合成配方目录（custom-craft-recipe §4.2）——唯一真源。
//
// 表注入、下标映射、行为分派全部由本目录派生；新增配方只需向 catalog() 追加一项，
// 不改动注入与 hook 代码。本文件为纯逻辑（不含游戏内存访问），可 host 单测。
namespace custom_recipe {

enum class Kind : uint8_t {
    kJewelTierUp,        // 目标宝石数值提升一档（模块改写放料/合成/产物）
    kNativePassThrough,  // 产物不改写：完全交给原版 + gemcraft 路径（模块三 hook 一律不介入）
    kThreeSlotCraft,     // 3 格隐式配方（宝石强化页 type 1）：按 3 格内容查表决定产物（§2.8/§4.12）
};

// 材料需求数量的计算规则（配方的固有属性，随 Def 声明）。
enum class CountRule : uint8_t {
    kFixed,               // 直接用 Material::count
    kJewelGradeAndLevel,  // ceil(Material::count × 宝石档位 × (105 − 角色等级) / 105)
};

// 配方形式：声明该配方借用哪个原版 UIMix type（0..4）的面板形态与提交流程。
// 配方按钮点击（UIMix_ButtonRecipeExe hook）时，若形式借用的 type ≠ 当前 type 则按此分派
// SetType（禁止借用 type 5：原版对 type>=5 走 ResetActiveControl 未初始化寄存器路径）。
enum class Form : uint8_t {
    kConsumeToCreate,   // 借用 type 4：直接消耗材料生成物品
    kTargetAndCreate,   // 借用 type 3：填入一个物品 + 消耗材料 → 生成/原地修改
    kMultiInputCreate,  // 借用 type 1：填入多个物品 → 生成
};

struct Material {
    uint16_t item_id;
    uint8_t count;
};

struct Def {
    uint16_t label_word_id;        // 配方按钮文本 wordId（RECIPEBASE b0-1）
    Kind kind;                     // 行为种类
    const Material* materials;     // 材料条目（MIXTUREBASE 记录）
    uint8_t material_count;        // 材料条目数（RECIPEBASE b6）
    uint16_t cost_word_id;         // 费用公式 wordId（0 = 免费；实际费用由 hook 强制 0）
    CountRule count_rule;          // 材料需求数量的计算规则（见 custom_recipe_rules.h）
    Form form;                     // 配方形式（借用的原版 type；见上）
    uint8_t group;                 // 页签组位索引（注入记录 b11 = 1 << group；须 < 8）
};

// 模块配方注入的描述文案（模块自有 UTF-8 字面量：36 字节 + NUL = 37，host 测试断言）。
// 取代原版拼装链对「注入记录结果占位 b2-3=0（ITEMDATABASE 记录 0=金币）」拼出的错误文案；
// 仅在 UIDesc_MakeItemByID 汇聚点写入游戏共享描述缓冲，**不写任何游戏文本数据**。
constexpr char kModuleRecipeDesc[] = "用3个材料合成";

// 「宝石强化」页签下 `Kind::kJewelTierUp` 条目的按钮文案 wordId。
// 该条目复用原版 35291 —— 它同时是**页签名**「宝石强化」（由 `UIMix_ButtonMenuListDraw` 绘制），
// 且文本表里不存在「宝石升阶」这一串。因此按钮实际显示由 `feature/ui/module_text` 的
// `recipe.jewel_tier_up` 条目在 `Scope::kUimixRecipeButton` 窗口内替换（页签不受影响）；
// 未挂上 hook 时退化为显示原版「宝石强化」，不崩、不影响功能。
// 本常量是**配方数据**（`Def::label_word_id` 取值）；module_text 侧的同名 id 由 host test
// （`test_module_text.cpp` 的 test_recipe_label_id_sync）断言与本常量一致，避免两处漂移。
inline constexpr uint16_t kJewelTierUpLabelWordId = 35291;

// 注入记录的 RECIPEBASE 结果物品 id（b2-3，u16）恒为 0（custom_recipe_table.h kRbResultId
// 「注入记录填 0」）。描述文案替换的门控依赖「当前 mixType 命中模块记录」这一条（另一条是
// 「被写的文本就是原版配方模板」），见 game_ui_custom_recipe.cpp 的 custom_set_desc_text_wrapper。
constexpr uint32_t kModuleRecipeResultItemId = 0;

// 3 格隐式配方（宝石强化页 UIMix type 1 的 3 个填入格；设计册 §2.8 / §4.12）。
// **匹配键 = 物品类别**（`item + I_TYPE` u16 的 bits6-15；取法同 `item_is_jewel`），
// 且对本项目用到的物品类别而言 **类别 == itemId**（文档类别表与 itemId 区间一致：
// 5-8/15 药水、16-25 卷轴、28-32 宝石；`ITEMDATABASE` 记录内无独立类别字段）。
//
// 槽位通配符：`kAnyJewelSlot` 表示「该格接受任意宝石类别（28..32）」，**只在 ordered
// （按槽位严格匹配）行里生效**；unordered 行参与排序比较，不得使用。
constexpr uint16_t kAnyJewelSlot = 0xFFFF;

// 产物生成方式。
enum class ProductMode : uint8_t {
    kFixedCategory,   // 产物 = `product` 类别新建（原版掷值），放料/合成语义不变
    kScaleFirstItem,  // 产物 = **第 1 格物品自身**（同类别），宝石数值 × `scale_permille/1000`，
                      // 并保留源物品的随机等级与属性类型（bits11-23 原样搬用）
};

struct ThreeSlotRecipe {
    uint16_t slots[3];
    bool ordered;      // true=按槽位严格匹配；false=按多重集（顺序无关）
    uint16_t product;  // kFixedCategory 的产物类别（kScaleFirstItem 时不使用，填 0）
    ProductMode product_mode;   // 产物生成方式
    uint16_t scale_permille;    // kScaleFirstItem 的数值缩放（千分比：1100 = ×1.1；其它模式填 0）
};

// 3 格配方表（唯一真源）；out_count 回传条目数。表序即匹配优先级（首个命中者胜出）。
const ThreeSlotRecipe* three_slot_recipes(size_t* out_count);

// 按 3 格类别（0=空）匹配；未命中返回 nullptr。ordered=false 用多重集比较。
const ThreeSlotRecipe* match_three_slot(const uint16_t slots[3]);

// 静态配方目录；out_count 回传条目数 N。**目录顺序即注入记录的 mixType 升序，也是
// MakeRecipeList 按记录下标写数组后的配方按钮顺序**（原版按 b11 命中记录的遍历序 = 下标序）。
const Def* catalog(size_t* out_count);

// 表注入完成并绑定原版记录数后为 true；未就绪时 def_for_mix_type 一律返回 nullptr。
bool catalog_ready();

// 绑定运行时原版 RECIPEBASE 记录数（注入时调用一次），使 mixType→Def 映射生效。
void bind_base_record_count(uint16_t base_record_count);

// 读回绑定的原版记录数（未绑定 = 0）。调用方配合 mix_type_at 推导「目录第 i 条对应的 mixType」。
uint16_t bound_base_record_count();

// mixType → 自定义配方 Def；非自定义配方（未注入 / 越界）返回 nullptr。
const Def* def_for_mix_type(uint32_t mix_type);

// 纯映射：配方 index 的 mixType = base_record_count + index。
uint32_t mix_type_at(uint16_t base_record_count, size_t index);

// 纯映射：配方 index 的材料起始下标 = base_material_count + Σ(material_count[0..index-1])。
uint32_t material_start_at(uint16_t base_material_count, const Def* cat, size_t cat_len,
                           size_t index);

// 纯映射：目录全部配方的材料条目总数 Σ material_count。
uint32_t material_total(const Def* cat, size_t cat_len);

}  // namespace custom_recipe
