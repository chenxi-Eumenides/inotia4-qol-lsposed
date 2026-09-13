#include "feature/gemcraft/gemcraft_rules.h"

namespace gemcraft {

bool is_jewel_category(int category) {
    return category >= 28 && category <= 32;
}

int first_empty_slot(const bool* filled, int slot_count) {
    if (filled == nullptr || slot_count <= 0) {
        return -1;
    }
    for (int index = 0; index < slot_count; ++index) {
        if (!filled[index]) {
            return index;
        }
    }
    return -1;
}

}  // namespace gemcraft
