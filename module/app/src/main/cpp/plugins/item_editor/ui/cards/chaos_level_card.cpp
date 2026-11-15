#include "cards.h"

#include "../panel_internal.h"
#include "numeric_card.h"

namespace qol::features::item_editor {
namespace {

// 混沌卡提供 ±100 增量，不提供“直接设为 100”。
constexpr int32_t kValueDeltas[6] = {-100, -10, -1, 1, 10, 100};

}  // namespace

bool open_chaos_level_card() {
    const ItemEditorDraft& draft = panel_internal::g_draft;
    if (!item_class_is_equipment(draft.class_index)) return false;
    NumericCardSpec spec{};
    spec.id = "chaos_level";
    spec.target = NumericCardTarget::kChaosLevel;
    spec.min_value = 0;
    spec.max_value = 100;
    spec.value = draft.chaos_level;
    spec.deltas = kValueDeltas;
    spec.delta_count = 6;
    spec.preset_value = 0;
    spec.has_preset = false;
    return numeric_card_open(spec);
}

}  // namespace qol::features::item_editor
