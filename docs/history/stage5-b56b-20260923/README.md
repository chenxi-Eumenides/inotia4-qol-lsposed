# 阶段 5 批 5-6b：世界写并入 core.world（读写一体）（VM-K5-06g）

日期：2026-09-23 ｜ 依据用户裁决：领域插件统一为 `core.<domain>`，**一个领域一个 owner、读写一体**。

## 1. 改动

- **世界写 5 条从 `world_teleport` 迁入 `core.world`**（读写一体）：
  `POST /api/world/movement/{move_to,walk_dir,stop_move,interact_with}`、`POST /api/op/movement/teleport`。
  - `plugins/core.world/plugin.cpp`：接收全部写侧构件（`kWorldWriteHandlerId=0x57540001`、`WorldWritePayload`、`world_int_field`/`world_bool_field`、`world_write_command`、`world_write_result`）+ 5 条声明式 POST 条目并入 `kHttpQueries`；`kPorts` 增 `QOL_PORT_WORLD_WRITE`（descriptor port_count 1→2）；install 校验 `ports[1]` 与 `abi.minor>=9`、注册结果处理器；deactivate 显式注销。
  - `plugins/world_teleport/plugin.cpp`：移除写侧全部构件与世界写端口需求（port_count 2→1、`http_queries`→`nullptr,0`），回归纯特性插件（消费 `ui_choice` + `world.facts`）。
- `host/plugin_registry.cpp`：`QOL_PORT_WORLD_WRITE` 私有约束 `world_teleport` → `core.world`。
- `plugins/core.api/routes.inc`：5 条 owner `world_teleport` → `core.world`（143 不变）。
- `contracts/game_ports/world_write_port.h`：注释归属更新为 `core.world`。
- 测试：`tests/test_world_facts_{plugin,dispatch}.cpp` 同步（port_count 2、http_query_count 12、POST 路由注册、世界写端口假实现、`ticket_result` 假面）。

## 2. 验证

| 项 | 结果 |
|---|---|
| Host CTest | **65/65** |
| `check_arch` / `check_http_routes` | 通过（143/143、exit=0）；`git diff --check` 干净 |
| 构建 | 普通 `output/inotia4_qol_lsposed_debug_2609232134_d392dfdd715d.apk`；LSPatch 3/3；已安装 |
| 真机·归属 | routes：`owner=core.world` **12 条**（7 GET + 5 POST），`world_teleport` **0 条**；features：`core.world`=enabled |
| 真机·写路由 | `stop_move` → 票据 → `done` + `{"ok":true}`；`interact_with` → 票据 → `done` + `{"ok":true,"state":{"money":28550,"map_id":20,"x":328,"y":136,"active_quest":0,"leader_slot":0,"party_count":3}}`（**与基线逐字一致**） |
| 真机·生命周期 | `core.world` deactivate → 7 GET + 2 抽样 POST 全 503 `http owner inactive`；activate → 全 200 |
| 真机·级联（既有语义） | 停用 `core.world`（provides `world.facts`）级联停用其消费者链：`inventory_hooks` → `autosell`/`extension_bag`；恢复需**按依赖顺序**逐个 `activate`（提供者→`inventory_hooks`→其消费者）；`simple_mode` 为配置门禁关闭的正常态 |
| smoke | `total=143 pass=121 warn=22 fail=0`，与基线 verdict/status **diff=0**；FATAL/SIGSEGV/SIGABRT=0 |

## 3. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `plugins/core.world/plugin.cpp`、`plugins/world_teleport/plugin.cpp`、`host/plugin_registry.cpp`、`plugins/core.api/routes.inc`、`contracts/game_ports/world_write_port.h`、`tests/test_world_facts_{plugin,dispatch}.cpp` | 无扩展背包 R-xx/B-xx | `VM-K5-06g` | 全部已执行：host/静态/构建/LSPatch/真机归属+写路由+生命周期+级联/smoke/logcat |

## 4. 遗留
- 其余领域（character/item/save/craft/system/quest/choice）按同一「core.<domain> 读写一体」模式后续批次统一。
- 级联恢复需显式逐级 activate 属既有设计（批 4 起登记），不影响 smoke。
