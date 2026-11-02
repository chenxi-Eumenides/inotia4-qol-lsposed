#pragma once

#include <cstdint>

#include "../../../contracts/ui.h"

namespace qol::features::core_ui {

class FrameOverlay {
public:
    struct Scope {
        uint64_t token = 0;
        const void* provider = nullptr;
        uint64_t generation = 0;
    };

    bool set_draw(contracts::QolUiFrameOverlayDrawFn draw, void* user);
    bool begin_scope(uint64_t token, const void* provider, uint64_t generation);
    void end_scope();
    bool draw_once(contracts::QolUiAdapter* ui);
    bool draw_once_for_scope(contracts::QolUiAdapter* ui, const Scope& expected);
    Scope current_scope() const;
    bool scope_live() const;

private:
    static constexpr uint32_t kMaxScopeDepth = 16;
    struct ScopeFrame {
        Scope scope{};
        bool blocked = false;
        bool consumed = false;
    };

    static bool same_scope(const Scope& left, const Scope& right);

    contracts::QolUiFrameOverlayDrawFn draw_ = nullptr;
    void* user_ = nullptr;
    ScopeFrame scopes_[kMaxScopeDepth]{};
    uint32_t scope_depth_ = 0;
    uint64_t overflow_depth_ = 0;
};

}  // namespace qol::features::core_ui
