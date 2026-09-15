#pragma once

// 合成器**界面层的 hook 挂载与分派**（craft_ui 域第二部分，见 craft_ui.h 的分层约定）。
//
// 职责：UIMix 的全部函数级 hook 都装在这里 —— 4 个按钮 ExecuteProc（放料/合成按钮/页签/配方点击）、
// 产物（MIXSYSTEM_MakeItem）、描述文本落点、填入格数量隐藏（ControlItem_Draw × ITEM_DrawPorting）、
// 模块文案的三个绘制窗口、以及「清空填入格」UIMix_ResetStuffItemControl。
//
// 与配方层的关系：wrapper 只做**界面判断与呈现**，一切「这三格是什么结果」都问
// feature/custom_recipe/custom_recipe_api.h，并按它回传的枚举决定弹什么文案、要不要清空。
// 本层不含配方知识；配方层不含任何弹窗/选中代码。依赖方向单向 craft_ui → custom_recipe。
//
// 线程模型：全部 wrapper 在游戏主线程（UIMix 事件/绘制路径）执行；安装可在任意线程调用，
// 但需 bridge 就绪。幂等、可重试（失败不半装核心链，装饰性 hook 失败只告警）。

// 安装全部 UIMix hook（幂等）。核心链（5 处）任一失败即返回 false 且可重试；
// 装饰性 hook（描述文案 / 数量隐藏 / 文案窗口 / 清空补选中）失败只告警、不拖垮核心链。
bool craft_ui_install_if_ready();

// 核心链是否已就绪。
bool craft_ui_hooks_installed();
