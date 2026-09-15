#pragma once

#include <cstddef>
#include <cstdint>

// 自定义合成配方目录（custom-craft-recipe §4.2）——唯一真源。
//
// 表注入、下标映射、行为分派全部由本目录派生；新增配方只需向 catalog() 追加一项，
// 不改动注入与 hook 代码。本文件为纯逻辑（不含游戏内存访问），可 host 单测。
namespace custom_recipe {

enum class Kind : uint8_t {
    kJewelTierUp,  // 目标宝石数值提升一档
};

// 材料需求数量的计算规则（配方的固有属性，随 Def 声明）。
enum class CountRule : uint8_t {
    kFixed,               // 直接用 Material::count
    kJewelGradeAndLevel,  // ceil(Material::count × 宝石档位 × (105 − 角色等级) / 105)
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
};

// 静态配方目录；out_count 回传条目数 N（本轮 N=1）。
const Def* catalog(size_t* out_count);

// 表注入完成并绑定原版记录数后为 true；未就绪时 def_for_mix_type 一律返回 nullptr。
bool catalog_ready();

// 绑定运行时原版 RECIPEBASE 记录数（注入时调用一次），使 mixType→Def 映射生效。
void bind_base_record_count(uint16_t base_record_count);

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
