#pragma once

#include <cstdint>

namespace qol::features::core_ui {

// 「更多游戏」主菜单替换控件的每帧决策纯逻辑。
//
// 游戏控件操作经 MoreGamesBindOps 注入。「非 main_menu 不碰控件」「主菜单控件重建后
// 重建替换」「绑定一致不重装」可在 host 用 fake 操作覆盖。
enum class MoreGamesBindingState : uint8_t {
    kUnknown,
    kNeedsReplace,
    kLive,
};

class MoreGamesBindOps {
public:
    virtual ~MoreGamesBindOps() = default;

    virtual MoreGamesBindingState binding_state() = 0;
    // False means identity/lifetime is unknown; caller must not install another component.
    virtual bool release() = 0;
    // 隐藏当前原版控件，并在同一父级、同一矩形创建组件文本按钮。
    virtual bool bind() = 0;
    // 原版 More Games 当前可解析（创建失败时用于区分 error 与 waiting）。
    virtual bool btn_resolvable() = 0;
    virtual void mark_live() = 0;
    virtual void mark_failed(bool btn_resolvable) = 0;
    virtual void mark_waiting() = 0;
};

// Unknown means identity/lifetime is not proven; retain the component and callback context.
// 非 main_menu 时只复位状态，不解引用或释放游戏拥有的菜单控件；回到主菜单后再校验实例。
void more_games_bind_tick(bool on_main_menu, MoreGamesBindOps& ops);

class MoreGamesDrawOps {
public:
    virtual ~MoreGamesDrawOps() = default;
    virtual bool replacement_live() = 0;
    virtual bool draw_replacement() = 0;
    virtual void draw_original() = 0;
};

// Returns true only when the component draw succeeded and the original can be suppressed.
bool more_games_draw(bool feature_enabled, MoreGamesDrawOps& ops);

class MoreGamesOriginalStateOps {
public:
    virtual ~MoreGamesOriginalStateOps() = default;
    virtual bool set_show(bool shown) = 0;
    virtual bool set_enabled(bool enabled) = 0;
    virtual bool set_active(bool active) = 0;
};

// Hide/deactivate is transactional. Failure restores the original boolean Active state before
// the replacement is discarded; callers may mark the binding live only on true.
bool more_games_disable_original(MoreGamesOriginalStateOps& ops, bool original_active);
// A false identity is a verified mismatch and performs no native writes.
bool more_games_restore_original(bool original_is_current, bool original_active,
                                 MoreGamesOriginalStateOps& ops);

}  // namespace qol::features::core_ui
