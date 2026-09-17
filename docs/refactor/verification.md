# 验证矩阵与验收卡

> 定位：定义「什么算通过」与「证据放哪里」。架构规则见根目录 `ARCHITECTURE.md`；执行状态见 `ledger.md`。

## 1. 交付定义

| 结论 | 条件 |
|---|---|
| **PASS** | 声明的验证项全部执行通过，且证据可复现（命令 + 输出/日志位置） |
| **PASS（host-only）** | 变更未触及行为面（纯逻辑/纯工具/骨架），host 测试通过即可，须在卡中标注范围 |
| **NOT_ACCEPTED** | 触及行为面但缺少真机证据；或以静态审查、Debug 构建、API 成功替代真机回归 |
| **BLOCKED** | 设备/环境不可用导致无法取证；必须写明待补项与复现命令 |

证据定义：**证据 = 命令 + 该命令的实际输出**；短输出内联在卡片内，长输出给出项目内可复现位置
（如 `docs/history/` 或 `archive/`）。仅有过程摘要、无命令或无输出者**不算证据**。

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

| 卡号 | 对象 | 命令（可复跑） | 实际输出（内联） |
|---|---|---|---|
| VM-P0-01 | 校验脚本族 + 基线棘轮 | `uv run python scripts/maintenance/check_arch.py` | `全部通过`（基线随迁移收缩：125→124→121）；`--freeze` 仅按规则前缀收缩，扩容需 `--force-freeze` |
| VM-P1-01 | `infra` 骨架（快照/命令队列/事件/能力/状态） | `cd module/app/src/main/cpp/tests && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure` | `100% tests passed, 0 tests failed out of 20`（P1 时点；当前 25/25）。范围：host-only，未接线运行时 |
| VM-P1-02 | 最小诊断接口 `GET /api/system/features` | `adb -s 192.168.3.54:5555 install -r output/inotia4_qol_lsposed_debug_2609170036_a8711e058093.apk && adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common && adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1 && curl -s http://192.168.3.54:8088/api/system/features` | `{"ok":true,"features":[{"id":"core","version":"0.1.0","state":"enabled","enabled":true,"error":null,"resource_count":0}]}`（`enter_slot` 后复测一致） |
| VM-P1-03 | `/api/health` 加法扩展 `ready` + `variant` | `curl -s http://192.168.3.54:8088/api/health` | `{"ok":true,"ready":true,"variant":{"series":"monster","version":"v27","known":true,"capabilities":3}}`；修复前日志 `game_variant: ... md5=cc517897dcaae8021e35a2426284c224 known=0` |
| VM-P2-01 | P2 纵向样板 `simple_mode`（入口 hook ×2 + 配置 + 状态 + 命令） | `curl -s http://192.168.3.54:8088/api/system/features`；`curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"simpleModeEnabled":true}'`；`curl -s -X POST http://192.168.3.54:8088/api/op/movement/teleport -H 'Content-Type: application/json' -d '{"map_id":34,"x":2,"y":20}'`；`curl -s http://192.168.3.54:8088/api/world/map/enemies` | 启动 `installed_disabled` + 两资源 `disabled`；开 → `enabled` + `live`（日志 `simple_mode enabled=1`）；关 → 回 `installed_disabled`（`enabled=0`）；**半血 A/B**：门禁开 `hp=135`，关门禁后刷图 `hp=270`（恰 2×）。伤害倍率子项：**非本卡证据**（历史归档，本轮未复测） |
| VM-P3-01 | P3 生命周期：`deactivate` 物理归还 / `activate` 重挂载 + 资源状态端点 | `curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"simple_mode","action":"deactivate"}'`；`curl -s http://192.168.3.54:8088/api/system/resources`；`curl -s http://192.168.3.54:8088/api/world/map/enemies` | deactivate `ticket=3` → `inactive` + 两资源 `free`；刷图 **`hp=270`**；activate `ticket=4` → `enabled` + `live`；刷图 **`hp=135`**；`GET /api/system/resources` 全程一致；无 FATAL。**U6 闭合：同址二次挂载安全** |
| VM-P3-02 | 资源事务故障注入与并发（host-only） | `cd module/app/src/main/cpp/tests && ctest --test-dir build --output-on-failure` | `100% tests passed, 0 tests failed out of 25`；`hooks_transaction: pass=277`、`plugin_lifecycle: pass=115`。覆盖：逆序回滚、`tombstone`+隔离、`resumable=false`、在途等待与帧预算超时、隔离拒绝、同插件单事务、`failed` 重试、非法迁移拒绝、`on_resource_ready` 失败全量回滚 |
| VM-P3-03 | P3.5：10 态状态 API、`degraded` 正交字段、tickets 端点 | `curl -s http://192.168.3.54:8088/api/system/features`；`curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"opEnabled":true}'`；`curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"simple_mode","action":"deactivate"}'`；`curl -s http://192.168.3.54:8088/api/system/tickets/2` | `features`：10 态名 + `degraded:false`；`tickets/2`：`{"ok":true,"ticket":2,"state":"pending"}` → 3 秒后 `{"ok":true,"ticket":2,"state":"done"}`，`inactive` + 两资源 `free`；`tickets/0`：`{"ok":false,"error":"invalid ticket"}`；`tickets/3` 在协议页帧冻结期停留 `pending`（帧驱动合法等待），关协议页帧号 `119→150` 后 `done`；无 FATAL |

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
