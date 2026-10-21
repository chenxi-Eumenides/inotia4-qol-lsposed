#include "frame_overlay.h"

#include "host.h"
#include "../ui_confirmation.h"

namespace qol::features::core_ui {

bool FrameOverlay::set_draw(contracts::QolUiFrameOverlayDrawFn draw, void* user) {
    if (draw == nullptr) {
        if (draw_ == nullptr) return true;
        if (user_ != user) return false;
        draw_ = nullptr;
        user_ = nullptr;
        return true;
    }
    if (draw_ != nullptr) return draw_ == draw && user_ == user;
    draw_ = draw;
    user_ = user;
    return true;
}

bool FrameOverlay::begin_scope(uint64_t token, const void* provider, uint64_t generation) {
    if (token == 0 || provider == nullptr || generation == 0) return false;
    if (scope_depth_ != 0 || overflow_depth_ != 0) {
        if (scope_depth_ < kMaxScopeDepth) {
            scopes_[scope_depth_++] = {{}, true, false};
        } else if (overflow_depth_ != UINT64_MAX) {
            ++overflow_depth_;
        } else {
            return false;
        }
        return true;
    }
    scopes_[scope_depth_++] = {{token, provider, generation}, false, false};
    return true;
}

void FrameOverlay::end_scope() {
    if (overflow_depth_ != 0) {
        --overflow_depth_;
        return;
    }
    if (scope_depth_ != 0) scopes_[--scope_depth_] = {};
}

bool FrameOverlay::draw_once(contracts::QolUiAdapter* ui) {
    if (scope_depth_ == 0) return false;
    return draw_once_for_scope(ui, scopes_[scope_depth_ - 1].scope);
}

bool FrameOverlay::draw_once_for_scope(contracts::QolUiAdapter* ui, const Scope& expected) {
    if (scope_depth_ == 0 || overflow_depth_ != 0 || draw_ == nullptr || ui == nullptr) {
        return false;
    }
    ScopeFrame& frame = scopes_[scope_depth_ - 1];
    if (frame.blocked || frame.consumed || !same_scope(frame.scope, expected)) return false;
    uint64_t active_token = 0;
    if (!ui_confirmation_modal_overlay_token(&active_token) || active_token != frame.scope.token ||
        !ui_panels_overlay_scope_matches(frame.scope.provider, frame.scope.generation)) {
        return false;
    }
    frame.consumed = true;
    try {
        return draw_(user_, ui, frame.scope.token);
    } catch (...) {
        return false;
    }
}

FrameOverlay::Scope FrameOverlay::current_scope() const {
    return scope_depth_ == 0 || scopes_[scope_depth_ - 1].blocked
        ? Scope{} : scopes_[scope_depth_ - 1].scope;
}
bool FrameOverlay::scope_live() const { return scope_depth_ != 0 || overflow_depth_ != 0; }

bool FrameOverlay::same_scope(const Scope& left, const Scope& right) {
    return left.token == right.token && left.provider == right.provider &&
        left.generation == right.generation;
}

}  // namespace qol::features::core_ui
