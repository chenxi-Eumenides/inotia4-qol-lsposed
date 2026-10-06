#pragma once

namespace qol::features::core_ui {

// 「更多游戏」按钮 ExecuteProc 绑定的每帧决策纯逻辑。
//
// 真实绑定操作（控件解析、槽读写、状态原子量）依赖游戏进程内存，无法进入 host
// 测试；决策面抽为纯逻辑、操作经 MoreGamesBindOps 注入后，「槽被游戏改写 → 下一帧
// 重绑」「非 main_menu 不保持 live 快照」「校验一致不重装」可在 host 用 fake 操作覆盖。
class MoreGamesBindOps {
public:
    virtual ~MoreGamesBindOps() = default;

    // 既有 token 记录的槽 ExecuteProc 是否仍指向本模块 replacement
    //（token 未持有时必须返回 false）。
    virtual bool proc_matches() = 0;
    // 释放绑定并复位状态到 waiting（unbind + 标志复位；无绑定时幂等）。
    virtual void release() = 0;
    // 安装绑定（写槽 + 记录 token/injected）；控件不可解析或写槽失败返回 false。
    virtual bool bind() = 0;
    // 控件当前是否可解析（绑定失败时用于区分 error 与 waiting）。
    virtual bool btn_resolvable() = 0;
    // 绑定在位：状态置 live。
    virtual void mark_live() = 0;
    // 绑定失败：btn_resolvable=true 置 error（控件在但写槽失败），否则回 waiting。
    virtual void mark_failed(bool btn_resolvable) = 0;
};

// 每帧 tick 决策：
// · 非 main_menu：释放绑定。游戏可随时重建主菜单控件（STATE_EnterMainMenu 重填槽），
//   保留 injected/bind=1 只会让 status 展示过时的 live；释放回 waiting，回到
//   main_menu 后由本 tick 重新绑定，且不长期占用已销毁控件的槽。
// · main_menu：校验一致 → live（无写操作）；槽被游戏改写或记录失效 → 释放后重装；
//   安装失败按控件可解析性落 error/waiting。
void more_games_bind_tick(bool on_main_menu, MoreGamesBindOps& ops);

}  // namespace qol::features::core_ui
