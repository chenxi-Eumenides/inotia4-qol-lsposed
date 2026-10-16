# `attribute_range_display` 阶段 5 人工目视 runbook（2026-10-17）

目标：确认属性详情行的**数值**按质量档位着色（H4 颜色码语义），并确认 required 依赖与守卫改动不破坏正常渲染。

设备/环境：`192.168.3.56:5555`；游戏包 `com.com2us.inotia4.normal.freefull.google.global.android.common`；API `http://192.168.3.56:8088`。当前已部署新模块 `02f8fa02377a…`，进入 `world`，`attribute_range_display` 与 `attribute_quality` 均 `enabled`、资源 `live`。

前置检查（主代理已确认）：`GET /api/system/features` 中两插件 `enabled`、`degraded=false`、资源 `live`；当前 `screen=world`。

## 卡 1：数值着色（核心）
- **目的**：确认物品详情里属性数值按档位着色，且标签/行结构正常。
- **前置**：已进入游戏世界；角色背包/装备里有一件带属性数值的装备或宝石（带词缀的装备、或独立宝石）。
- **操作**：打开该物品详情（背包/装备页点物品），观察属性行如 `标签: 数值`。
- **预期**：数值前有颜色；档位映射 `Grey→G / White→W? / Green→C / Blue→L / Purple→V / Gold→Y`（`feature.cpp:35-51`，字母为内联色码，非最终颜色名）。行内标签与数值均可读，无溢出/乱码/错行；重复 id 的两行按各自当前数值独立着色。
- **失败判据**：数值无着色（一直原色）、行渲染残缺、乱码、错位、详情打不开、崩溃。
- **取证**：截图（物品详情）；如异常，主代理同步抓 logcat。
- **预计时长**：2–3 分钟。

## 卡 2：开关关闭后回归原色（可选）
- **目的**：确认 feature 关闭（生命周期停用）后无着色、恢复原版渲染。
- **前置**：卡 1 通过。
- **操作**：主代理执行 `POST /api/op/features/lifecycle {"id":"attribute_range_display","action":"deactivate"}`；你重新打开同一物品详情。
- **预期**：数值恢复原色，行结构完好；再 `activate` 后卡 1 现象回来。
- **失败判据**：停用后仍着色、或行渲染异常。
- **取证**：两张详情截图（停用前/后）。
- **预计时长**：2 分钟。

## 回滚
- 主代理：`POST /api/op/features/lifecycle` 将两插件恢复 `activate`；必要时重装集成包并重启。
- 若出现崩溃/卡死：主代理抓 `debuggerd -b <pid>` + logcat，按事故分支处理。

## 结论登记
- 每张卡记录「通过/失败 + 现象 + 截图路径」，由主代理登记入 `docs/refactor/audits/plugin-audit/attribute-range-display-20261017/verification-log.md` §3 与 `docs/refactor/verification.md` §5.19（VM-ARD-01）。
