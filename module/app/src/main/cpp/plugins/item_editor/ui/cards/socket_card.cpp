#include "cards.h"

#include "../panel_internal.h"
#include "numeric_card.h"

#include <cstdio>

namespace qol::features::item_editor {
namespace {

// 数值卡 4 个增量按钮：±1/±10，与原数字卡按钮顺序一致（-大/-小/+小/+大）。
constexpr int32_t kValueDeltas[4] = {-10, -1, 1, 10};

}  // namespace

bool open_socket_card() {
    const ItemEditorDraft& draft = panel_internal::g_draft;
    if (!item_class_is_equipment(draft.class_index)) return false;
    // 下限是已镶宝石数，防止把孔数改到低于 filled；上限用原生/UI 的 15。
    // 理论上 filled≤15，越界时只允许 max 并记录失败日志。
    int32_t min_value = draft.socket_filled;
    if (min_value > 15) {
        char message[96]{};
        std::snprintf(message, sizeof(message),
            "fail op=socket_card_open result=failed reason=filled_exceeds_max filled=%d",
            min_value);
        panel_internal::log_panel(message);
        min_value = 15;
    }
    NumericCardSpec spec{};
    spec.id = "socket_total";
    spec.target = NumericCardTarget::kSocketTotal;
    spec.min_value = min_value;
    spec.max_value = 15;
    spec.value = draft.socket_total;
    spec.deltas = kValueDeltas;
    spec.delta_count = 4;
    return numeric_card_open(spec);
}

}  // namespace qol::features::item_editor
