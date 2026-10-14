// custom_recipe UI 子层：放料处置映射门禁（合并后内部结果枚举；不再依赖外部 craft_result 能力）。
#include <cassert>

#include "plugins/custom_recipe/result.h"
#include "plugins/custom_recipe/ui/feature.h"

int main() {
    using qol::features::custom_recipe::result::CraftGate;
    using qol::features::custom_recipe::result::PlaceOutcome;
    using qol::features::custom_recipe::ui::CraftPlaceDisposition;
    using qol::features::custom_recipe::ui::craft_place_disposition;

    // 写入失败必须与「已放置」/「停下」区分：调用方据此不推进选中格。
    assert(craft_place_disposition(PlaceOutcome::kWriteFailed) ==
           CraftPlaceDisposition::kWriteFailed);
    assert(craft_place_disposition(PlaceOutcome::kWriteFailed) !=
           CraftPlaceDisposition::kAdvance);
    assert(craft_place_disposition(PlaceOutcome::kWriteFailed) != CraftPlaceDisposition::kStop);
    // 成功放置 → 推进到下一个空格。
    assert(craft_place_disposition(PlaceOutcome::kPlaced) == CraftPlaceDisposition::kAdvance);
    // 同对象重复放置 / 持有总数不足：保持已填格（不回退到 stop）。
    assert(craft_place_disposition(PlaceOutcome::kAlreadyPlaced) ==
           CraftPlaceDisposition::kAlreadyPlaced);
    assert(craft_place_disposition(PlaceOutcome::kNotEnoughHeld) ==
           CraftPlaceDisposition::kNotEnoughHeld);
    // 非本层配方 → 停下（转调原版）。
    assert(craft_place_disposition(PlaceOutcome::kNotMine) == CraftPlaceDisposition::kStop);

    // CR02：质量服务不可用必须与「真正非宝石」区分（不同提示），且只阻断宝石升阶。
    assert(CraftGate::kUnavailable != CraftGate::kNotJewel);
    assert(CraftGate::kPass != CraftGate::kUnavailable);
    assert(static_cast<uint8_t>(CraftGate::kUnavailable) == 3);
    return 0;
}
