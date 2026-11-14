#include "cards.h"

#include "../panel_internal.h"
#include "../../plugin.h"
#include "numeric_card.h"

namespace qol::features::item_editor {
namespace {

// 数量卡 8 个增量按钮：±1/±10/±100/±1000，行优先两行布局；标签由通用卡按 %+d 生成。
constexpr int32_t kQuantityDeltas[8] = {-1, -10, -100, -1000, 1, 10, 100, 1000};

}  // namespace

bool open_quantity_card() {
    const ItemEditorDraft& draft = panel_internal::g_draft;
    if (!item_class_valid(draft.class_index) ||
        item_class_is_equipment(draft.class_index) || !draft.stackable) return false;
    NumericCardSpec spec{};
    spec.id = "quantity";
    spec.target = NumericCardTarget::kCount;
    // 上限按类别 stack_limit 决定（99/999），下限固定 1。
    spec.min_value = 1;
    spec.max_value = stack_limit_max_count();
    spec.value = draft.count;
    spec.deltas = kQuantityDeltas;
    spec.delta_count = 8;
    return numeric_card_open(spec);
}

}  // namespace qol::features::item_editor
