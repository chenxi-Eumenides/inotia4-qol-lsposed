#pragma once

#include <cstdint>

#include "../../contracts/item_handle.h"
#include "../../contracts/result.h"

// `custom_recipe` 内部结果层与 UI 子层之间的 typed contract。
// 不经 capability registry 对外提供。
namespace qol::features::custom_recipe::result {

enum class PlaceOutcome : uint8_t {
    kNotMine = 0,
    kPlaced = 1,
    kAlreadyPlaced = 2,
    kNotEnoughHeld = 3,
    kWriteFailed = 4,
};

enum class PrepareOutcome : uint8_t {
    kNotMine = 0,
    kNoMatch = 1,
    kConfirmPending = 2,
    // 尾追加：配方已匹配，但读取槽位身份/类别等上下文失败（非「配方不匹配」）。
    kUnavailable = 3,
};

enum class CraftOutcome : uint8_t {
    kOk = 0,
    kStaleSlots = 1,
    kNotEnoughHeld = 2,
    kNoBagSpace = 3,
};

enum class CraftExecuteResult : uint8_t {
    kOk = 0,
    kStaleSlots = 1,
    kNotEnoughHeld = 2,
    kNoBagSpace = 3,
    kAborted = 4,
    kPrecheckFailed = 5,
    kProductPrepareFailed = 6,
    kMaterialRemoveFailed = 7,
    kMaterialObservationFailed = 8,
    kProductSaveFailed = 9,
};

enum class CraftGate : uint8_t {
    kPass = 0,
    kNotJewel = 1,
    kAlreadyGold = 2,
    kUnavailable = 3,
};

struct CraftPlaceInput {
    int64_t slot_index;
    contracts::QolItemHandle item;
    bool item_stackable;
    uint16_t item_category;
    int placed_same_object;
    int same_category_placed;
    int category_held_count;
};

struct CraftPrepareInput {
    uint16_t slot_categories[3];
};

struct CraftExecuteInput {
    uint16_t slot_categories[3];
    contracts::QolItemHandle items[3];
    int held_counts[3];
};

struct CraftMaterialNeedInput {
    uint32_t mix_type;
    int jewel_grade;
    int character_level;
};

struct CraftMaterialNeedOutput {
    int needs[8];
    int count;
};

struct CraftMakeItemInput {
    uint32_t mix_type;
    contracts::QolItemHandle item;
    QolResult (*rand_inclusive)(int32_t lo, int32_t hi, int32_t* out);
};

struct CraftResultApi {
    bool (*enabled)();
    bool (*three_slot_mode_active)(uint32_t mix_type);
    bool (*is_jewel_tier_up_recipe)(uint32_t mix_type);
    int64_t (*form_ui_type_for_mix_type)(uint32_t mix_type);
    CraftGate (*craft_gate)(contracts::QolItemHandle target_item);
    bool (*is_jewel_item)(contracts::QolItemHandle item);
    PlaceOutcome (*place)(const CraftPlaceInput* input);
    PrepareOutcome (*prepare)(const CraftPrepareInput* input);
    CraftExecuteResult (*execute)(const CraftExecuteInput* input);
    void (*material_needs)(const CraftMaterialNeedInput* input,
                           CraftMaterialNeedOutput* output);
    int32_t (*make_item)(const CraftMakeItemInput* input);
    bool (*desc_should_replace)(uint32_t mix_type, const char* text);
};

}  // namespace qol::features::custom_recipe::result
