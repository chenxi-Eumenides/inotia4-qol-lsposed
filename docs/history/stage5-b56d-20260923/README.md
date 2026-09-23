# 阶段 5 批 5-6d：存档核心写入并入 core.save（裁决 A）（VM-K5-06i）

日期：2026-09-23 ｜ 依据用户裁决 A：3 条存档核心写并入同域 owner `core.save`；4 条文件级备份端点保留在 `save_backup`（其独立特性 + 自带 UI 面板）。

## 1. 改动

**`plugins/core.save/plugin.cpp`**（读面 → 读写一体）
- 新增写面：`SaveWriteRoute{kSave,kEnterSlot,kCreateSlot}`、`kSaveWriteHandlerId = 0x53420001`（随代码从 save_backup 迁入，全局 id 空间不变）、`SaveWritePayload{kind,slot,class_idx}`、`cs_http_error/cs_http_body/cs_extract_int`、`save_write_result_handler`（主线程执行含 **延迟结算** 与 sidecar 生命周期）、`cs_save_write_command`（校验投递）、`cs_http_handler`、`kSaveWriteRoutes[3]`（声明式 POST）。
- 端口 `kPorts` 增 `QOL_PORT_SAVE_WRITE`；install 校验 `port_count==2`、端口版本、`abi.minor>=9` 与尾部结果面，并注册结果处理器；deactivate 显式注销；descriptor `port_count 1→2` + `http_queries=kSaveWriteRoutes,3`。

**`plugins/save_backup/plugin.cpp`**（回到纯备份特性）
- 移除写面（enum 值、handler id、payload、结果处理器、`sb_write_command`、`g_sdk`/`g_save_write`、3 条路由条目与 handler 分支、`kPorts` 的 SAVE_WRITE、descriptor `port_count 2→1`）；`kSbHttpRoutes` 保持 4 条 backup 端点（声明式，宿主托管）。
- 保留 `bundle.cpp` 备份逻辑与面板 provider。

**接线与契约**
- `host/plugin_registry.cpp`：`QOL_PORT_SAVE_WRITE` 私有约束 `save_backup`→`core.save`。
- `plugins/core.api/routes.inc`：3 条 owner `save_backup`→`core.save`（143 不变）。
- `contracts/game_ports/save_write_port.h`：归属注释改 `core.save`。
- 测试：`tests/test_save_facts_plugin.cpp` 期望 `port_count==2`（SAVE + SAVE_WRITE）、`http_query_count==3`。

## 2. 验证

| 项 | 结果 |
|---|---|
| Host CTest | **65/65** |
| `git diff --check` / `check_arch` / `check_http_routes` | 通过（routes 143/143、exit=0） |
| 构建 | `output/inotia4_qol_lsposed_debug_2609232205_13a19fde6325.apk`；LSPatch 3/3；已安装 |
| 真机·owner | `/api/system/{save,enter_slot,create_slot}` owner 全 `core.save`；`core.save` 与 `save_backup` 均 enabled |
| 真机·保真 | `enter_slot {slot:0}` → 200 `{"ok":true,"ticket":8}` → `state:"done"`、`result` = `{"ok":true,"state":{"money":0,"map_id":4294967295,"x":-1,"y":-1,"active_quest":0,"leader_slot":0,"party_count":0}}`（**与三份迁移前落盘证据逐字一致**）、`result_retained:true` |
| 真机·校验前置 | `enter_slot {}` → 400 `slot required (0-2)`；`create_slot {slot:0}`（缺 class_idx）→ 400 |
| 真机·墓碑 | deactivate `core.save` → `save` 503、`enter_slot`（合法 body）503 `http owner inactive`；activate 后恢复 200 |
| 真机·级联 | 停用 `core.save`（provides `save.facts`）级联停用 `save_backup` 与 `extension_bag`；按依赖序 activate（core.save → save_backup → extension_bag）后全部 `enabled` |
| API smoke | `total=143 pass=125 warn=18 fail=0`；与基线 verdict/status diff 仅 4 条已知 party op 环境差异 |
| logcat | FATAL/SIGSEGV/SIGABRT = 0 |

## 3. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `plugins/core.save/plugin.cpp`、`plugins/save_backup/plugin.cpp`、`host/plugin_registry.cpp`、`plugins/core.api/routes.inc`、`contracts/game_ports/save_write_port.h`、`tests/test_save_facts_plugin.cpp`、`docs/refactor/{api-route-map,verification,ledger}.md`、`docs/reference/api-reference.md` | 无扩展背包 R-xx/B-xx | `VM-K5-06i` | 全部已执行：host/静态/构建/LSPatch/真机（owner、保真、校验、墓碑、级联）/smoke/logcat |

## 4. 备注

- typed handler id 仍为 `0x53420001`，仅注册者由 `save_backup` 变为 `core.save`（同一时刻只有一个注册者）。
- 级联恢复顺序（save.facts 消费者）已在验收中记录：`core.save` → `save_backup` → `extension_bag`。
