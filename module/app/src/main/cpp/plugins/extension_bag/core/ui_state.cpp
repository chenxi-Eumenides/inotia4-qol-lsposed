#include "plugins/extension_bag/core/ui_state.h"

namespace qol::extension_bag {

UiState& ui_state() {
    static UiState state{};
    return state;
}

}  // namespace qol::extension_bag

thread_local ModuleUseToken* g_active_module_use_token = nullptr;
