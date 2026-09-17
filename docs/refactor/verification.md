# 验证矩阵与验收卡

> 定位：定义「什么算通过」与「证据放哪里」。架构规则见根目录 `ARCHITECTURE.md`；执行状态见 `ledger.md`。

## 1. 交付定义

| 结论 | 条件 |
|---|---|
| **PASS** | 声明的验证项全部执行通过，且证据可复现（命令 + 输出/日志位置） |
| **PASS（host-only）** | 变更未触及行为面（纯逻辑/纯工具/骨架），host 测试通过即可，须在卡中标注范围 |
| **NOT_ACCEPTED** | 触及行为面但缺少真机证据；或以静态审查、Debug 构建、API 成功替代真机回归 |
| **BLOCKED** | 设备/环境不可用导致无法取证；必须写明待补项与复现命令 |

证据位置约定：真机证据写入 `.tmp/<task>/` 后迁 `docs/history/` 或 `archive/`；脚本证据以命令与输出为准（可复跑）。

## 2. 测试分层

| 层 | 手段 | 覆盖 |
|---|---|---|
| 纯逻辑 | host 单测（`cpp/tests`，CMake + ctest） | rules、编码、几何、协议解析、快照/队列/事件/能力/状态 |
| 装配与失败路径 | host + fake 后端 | 安装/停用、资源冲突、部分失败与回滚、重复 enable/disable、缺依赖降级、契约版本不匹配 |
| 并发与切换 | host + fake | 配置热切换、迁移中并发切换、在途回调期 deactivate、帧预算超时 |
| 真机 | 每插件一张验收卡 | ABI、时序、交互、变体差异、崩溃兜底 |

## 3. 验收卡模板（插件级）

```
卡号：VM-<域>-<序号>        阶段：P2/P3/P4
覆盖插件/资源：<plugin id> / <resource id>
前置：变体=<原版|大修|monster>；开关=<key=值>；存档槽=<slot>
步骤：1) … 2) … 3) …
观测：屏幕状态 / API 响应字段 / 日志锚（domain + 关键字）
失败判据：安装失败未上报 / 状态与开关不符 / 崩溃 / 静默无效
证据：<命令与输出文件路径>
结论：PASS / NOT_ACCEPTED / BLOCKED + 原因
```

## 4. 已建立的验收卡

| 卡号 | 对象 | 方法 | 结果与证据 |
|---|---|---|---|
| VM-P1-01 | `infra` 骨架（快照/命令队列/事件/能力/状态） | host：`cmake --build build && ctest --test-dir build` | PASS（20/20，含快照并发撕裂断言）；范围：host-only，未接线运行时 |
| VM-P1-02 | 最小诊断接口 `GET /api/system/features` | 真机：安装 debug APK → force-stop → monkey → `dialog/select {"action":"ok"}` → `curl` | PASS（HTTP 200；`{"ok":true,"features":[{"id":"core","version":"0.1.0","state":"enabled",...}]}`；`enter_slot 0` 后复测一致） |
| VM-P1-03 | `/api/health` 加法扩展 `ready` + `variant` | 真机：安装 debug APK → force-stop → monkey → `curl /api/health` | PASS（`{"ok":true,"ready":true,"variant":{"series":"monster","version":"v27","known":true,"capabilities":3}}`） |
| VM-P2-01 | P2 纵向样板 `simple_mode`（入口 hook ×2 + 配置 + 状态 + 命令） | 真机：安装 debug APK → 启动 → 关协议页 → `enter_slot 0` → `curl /api/system/features` → `POST /api/config/set` 两态 → 传送到怪物图读 `/api/world/map/enemies` 做 A/B | PASS（启动 `installed_disabled`/两条资源 `disabled`；开 → `enabled`/两条 `live` + 日志 `simple_mode enabled=1`；关 → 回 `installed_disabled`/`disabled` + 日志 `enabled=0`；**半血 A/B**：门禁开时 老鼠/喝醉的蝙蝠 lvl3 `hp=135`，关门禁后刷图 `hp=270`（恰 2×）；无 FATAL/崩溃；基线 124→121 收缩）。范围说明：伤害倍率（×1.5/×0.5）本轮未重复定量测量，规则层由 host 断言覆盖（33 条），历史真机证据见 `archive/docs/development/features/simple-mode.md` |
| VM-P3-01 | P3 生命周期全路径：`deactivate` 物理归还 / `activate` 重挂载 + 资源状态端点 | 真机：安装 debug APK → 进世界 → `config/set {"simpleModeEnabled":true}` → 刷图读 `/api/world/map/enemies` → `POST /api/op/features/lifecycle` 两次（`opEnabled` 临时开启） | PASS（门禁开 `hp=135`；`deactivate` ticket=3 → `inactive` + 两资源 `free`；**物理归还后刷图 `hp=270`**；`activate` ticket=4 → `enabled` + 两资源 `live`；**重挂载后刷图 `hp=135`**；`GET /api/system/resources` 全程一致；无 FATAL）。**U6 闭合：unhook 后重新挂载安全** |
| VM-P3-02 | 资源事务故障注入与并发（host-only） | `ctest`：`hooks_transaction_tests`（13 用例/277 断言）+ `plugin_lifecycle_tests`（7 用例/84 断言） | PASS(host-only)：安装失败逆序回滚、回滚失败→`tombstone`+隔离、`resumable=false`、在途等待与 `step_budget` 超时、隔离拒绝、同插件单事务、`failed` 显式重试、busy/非法迁移拒绝 |
| VM-P0-01 | 校验脚本族 + 基线棘轮 | 脚本：`check_arch.py`（含 `--freeze` 拒绝扩容验证） | PASS（124 条基线；新增违规失败；已消除条目可收缩：125→124） |

> VM-P1-03 执行时发现并修复线上缺陷：变体表停在 monster v26，设备 v27 查表失败
> （`known=0`、能力位 0，靠 `WH4JRN01` 回退分类）；重新生成后补入 v27/v29，设备命中
> `known=1, version=v27, caps=0x3`。该修复需同步到主线（见 `ledger.md` §5）。

## 5. 待建立

| 阶段 | 卡片 | 说明 |
|---|---|---|
| P4 | 每插件一张迁移卡 | 行为等价回归 + 开关两态 + 变体矩阵 |
| P6 | 外部插件轨卡 | 导入/拒绝/禁用/崩溃兜底 |

## 6. 失败判据（任一命中即 NOT_ACCEPTED）

1. 安装/停用结果与状态上报不一致（静默失败）；
2. 开关关闭后仍有业务副作用（非空转透传）；
3. 同一地址出现第二个安装者或二次挂载 rc=-1；
4. 非主线程调用写副作用游戏函数；
5. 崩溃 / `UnsatisfiedLinkError` / 未捕获异常冒泡到游戏进程；
6. 证据只有静态审查、构建成功或 API 200，缺少可复现的行为观测。
