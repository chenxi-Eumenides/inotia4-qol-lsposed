#include "cards.h"

#include "../panel_internal.h"
#include "numeric_card.h"

namespace qol::features::item_editor {
namespace {

// 数值卡 4 个增量按钮：±1/±10，与原数字卡按钮顺序一致（-大/-小/+小/+大）。
constexpr int32_t kValueDeltas[4] = {-10, -1, 1, 10};

}  // namespace

bool open_chaos_rate_card() {
    const ItemEditorDraft& draft = panel_internal::g_draft;
    if (!item_class_is_equipment(draft.class_index)) return false;
    NumericCardSpec spec{};
    spec.id = "chaos_rate";
    spec.target = NumericCardTarget::kChaosRate;
    spec.min_value = 0;
    spec.max_value = 255;
    spec.value = draft.chaos_rate;
    spec.deltas = kValueDeltas;
    spec.delta_count = 4;
    spec.preset_value = 100;
    spec.has_preset = true;
    return numeric_card_open(spec);
}

}  // namespace qol::features::item_editor
