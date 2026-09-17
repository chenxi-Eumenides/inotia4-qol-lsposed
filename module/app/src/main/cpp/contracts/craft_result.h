#pragma once

#include <cstdint>

// 自定义合成「结果层」能力契约（ARCHITECTURE.md §3.4；C2 Step 2）。
//
// 定位：把 feature/custom_recipe 的合成结果层（3 格隐式配方 + 宝石升阶）暴露为只读/纯决策
// 能力，供界面层 feature/craft_ui 作为消费者经能力注册表获取，消除 craft_ui ↔ custom_recipe
// 的源码级双向 include（C2 Step 3/4 接线；本文件当前只声明、不注册）。
//
// 边界（界面归界面，结果归结果）：
//   * provider 不读 UIMix/控件：所有界面状态（选中的库存物品、三格内容、持有数）一律由
//     调用方放入 Input 结构体携带；provider 只做判定与结果计算。
//   * provider 不弹窗、不改选中格、不播声音；调用方按返回枚举决定界面动作。
//
// 契约类型限制同 contracts/plugin.h：POD + 固定宽度 C 类型 + 函数指针；禁止 STL。

namespace qol::contracts {

constexpr const char* kCraftResultCapabilityId = "craft_result";
constexpr int32_t kCraftResultVersionMajor = 1;
constexpr int32_t kCraftResultVersionMinor = 0;

// 放料（3 格隐式配方）结果（与 feature/custom_recipe 现枚举同名同值）。
enum class PlaceOutcome : uint8_t {
    kNotMine = 0,        // 不是 3 格模式 → 调用方转调原版
    kPlaced = 1,         // 已放入；调用方随即选中下一空格
    kAlreadyPlaced = 2,  // 不可堆叠物品重复放置 → 调用方弹 99
    kNotEnoughHeld = 3,  // 可堆叠：类别持有总数不足 → 调用方弹 94（不清空已填格）
};

// 合成键前置（3 格隐式配方）结果。
enum class PrepareOutcome : uint8_t {
    kNotMine = 0,         // 不是 3 格模式 → 调用方转调原版
    kNoMatch = 1,         // 三格不匹配任何配方 → 调用方弹 94 + 清空 + 补选中
    kConfirmPending = 2,  // 已置待合成条目 → 调用方弹确认框（确认后调 execute）
};

// 合成执行（确认后）结果。
enum class CraftOutcome : uint8_t {
    kOk = 0,             // 成功 → 调用方走成功收尾
    kStaleSlots = 1,     // 确认期间格被改动 / 停用 / 无待确认 → 调用方弹 94 + 清空 + 补选中
    kNotEnoughHeld = 2,  // 扣料前库存复核失败（未消耗）→ 调用方弹 94（保留已填格）
    kNoBagSpace = 3,     // 产物创建/入包失败（材料已扣）→ 调用方弹 5 + 清空 + 补选中
};

// 合成执行的契约返回值：在 CraftOutcome 基础上追加 kAborted（停用/无待确认/入参失效，
// 明确区别于「格子已变」的 kStaleSlots；供 Step 3/4 provider 使用）。
enum class CraftExecuteResult : uint8_t {
    kOk = 0,
    kStaleSlots = 1,
    kNotEnoughHeld = 2,
    kNoBagSpace = 3,
    kAborted = 4,
};

// kJewelTierUp（原版「宝石强化」条目）合成键前置结果。
enum class CraftGate : uint8_t {
    kPass = 0,         // 放行 → 调用方转调原版
    kNotJewel = 1,     // 目标槽非宝石 → 调用方弹 97
    kAlreadyGold = 2,  // 已达金档 → 调用方弹 105
};

// ---- 入参（界面状态一律携带；provider 不读 UIMix）----

// 放料：调用方读好的选中物品与库存状态。
struct CraftPlaceInput {
    int64_t slot_index;          // 目标填入格 0..2
    void* item;                  // 选中的库存物品（nullptr = 无事发生）
    bool item_stackable;         // 是否可堆叠
    uint16_t item_category;      // item 的类别（item+I_TYPE bits6-15）
    int placed_same_object;      // 该对象已在其它格出现的次数
    int same_category_placed;    // 该类别已占格数（可堆叠上限判定）
    int category_held_count;     // 该类别当前持有总数（INVEN_GetItemCount）
};

// 前置：调用方读好的三格类别（0 = 空）。
struct CraftPrepareInput {
    uint16_t slot_categories[3];
};

// 执行：确认回调时刻重读的三格内容（provider 不读控件）。
struct CraftExecuteInput {
    uint16_t slot_categories[3];  // 三格类别（0 = 空）
    void* items[3];               // 三格物品指针（nullptr = 空）
    int held_counts[3];           // 与 items 各格类别对应的持有总数（可堆叠复核用）
};

// 材料需求：按 mix_type + 宝石档位 + 角色等级计算。
struct CraftMaterialNeedInput {
    uint32_t mix_type;
    int jewel_grade;      // 1..5；<=0 = 无法识别（provider 不改写需求）
    int character_level;  // 角色等级
};

struct CraftMaterialNeedOutput {
    int needs[8];  // 各材料格需求数量（按材料条目顺序）
    int count;     // 有效条目数
};

// 产物（宝石升阶）：mix_type 命中时原地改写 item。
struct CraftMakeItemInput {
    uint32_t mix_type;
    void* item;                            // 目标宝石物品（原地读改写数值位）
    int64_t (*rand_inclusive)(int, int);   // 闭区间随机源（生产用 MATH_GetRandom）
};

// ---- 能力函数表（vtable）----

struct CraftResultApi {
    bool (*enabled)();
    // mix_type 是否属于 3 格隐式配方模式（总开关 + kind 判定）。
    bool (*three_slot_mode_active)(uint32_t mix_type);
    // mix_type 是否为 kJewelTierUp（宝石升阶）配方。
    bool (*is_jewel_tier_up_recipe)(uint32_t mix_type);
    // mix_type 借用的原版 UI type（0..4）；-1 = 非模块配方。
    int64_t (*form_ui_type_for_mix_type)(uint32_t mix_type);
    // kJewelTierUp 合成键前置判定；target_item 为当前目标槽物品（调用方读好后传入）。
    CraftGate (*craft_gate)(void* target_item);
    bool (*is_jewel_item)(void* item);
    PlaceOutcome (*place)(const CraftPlaceInput* input);
    PrepareOutcome (*prepare)(const CraftPrepareInput* input);
    CraftExecuteResult (*execute)(const CraftExecuteInput* input);
    void (*material_needs)(const CraftMaterialNeedInput* input, CraftMaterialNeedOutput* output);
    // 返回 <0 = 非本层配方；0 = 已改写；>0 = 失败。
    int32_t (*make_item)(const CraftMakeItemInput* input);
    bool (*desc_should_replace)(uint32_t mix_type, const char* text);
};

}  // namespace qol::contracts
