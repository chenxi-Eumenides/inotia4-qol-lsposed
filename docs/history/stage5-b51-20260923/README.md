# 阶段 5 批 5-1：extension_bag / core.ui 写端点 owner dispatch

日期：2026-09-23 ｜ 对应卡：VM-K5-05b ｜ 范围：已有真实 owner 的 11 条 POST（EB 7 + core.ui 4）。

## 1. 范围与 owner

| 路由 | owner |
|---|---|
| POST `/api/debug/extension_bag/{enter_view,exit_view,unequip,select_bag,click_item,equip,item}` | `extension_bag` |
| POST `/api/debug/settings-ui/{inject,restore,open-option,open-panel}` | `core.ui` |

`routes.inc` 不改（owner 已真实）；143 条、method/path/op_gated/lease_required 不变。
`/api/debug/settings-ui/status`（GET）与其 owner 的查询面保持原状。

## 2. 实现要点

- **command 注册（自注册模式，core.api lifecycle 先例）**：两个插件 `plugin_install` 尾部逐条
  `sdk->http_dispatch.register_command`；**不写 descriptor `kHttpQueries`**（其 `http_query_count`
  仍是 0，宿主的 `unregister_http_queries` 对 descriptor 声明的条目早退，故摘除责任在插件）。
- **批量注册分流**：`http_dispatch_register_queries` 由「一律 register_query」改为按 method 分流
  （kPost → register_command），供将来声明式 command 条目使用；本次两个插件走自注册。
- **handler 直调插件内业务函数**：EB 7 条 → `data_op_extension_bag_*` / `data_virtual_bag_test_*`
  （`plugins/extension_bag/plugin.cpp` 私有 `extract_int_field` 宽松提取 body 整数字段）；
  core.ui 4 条（无 body）→ `data_settings_ui_{inject,restore,open_option,open_panel}`。
  响应体来自同一业务函数，形状天然冻结。
- **校验语义**：`bag ∈ [6,10]` → 400 `bag required (6-10)`；`click_item` 的 `slot ∈ [0,15]` →
  400 `slot required (0-15)`（与迁移前 Kotlin `ApiException` 文本逐字一致）。
  **body 合法性校验保留在 Kotlin 传输层**（`DebugController.postEb` 的 `JsonUtil.parseBody`
  → 400 `bad request`，合法则归一化 `json.toString()` 透传）——与迁移前对非法 body 的 400 行为
  逐字一致；native 侧不做 JSON 语法拒绝，只做字段提取。
- **显式摘除（关键修复）**：两个插件的 `plugin_deactivate` 必须显式
  `http_dispatch.unregister_owner(kPluginId)`。因 handler 由插件自注册、不在 descriptor 中，
  宿主的 `unregister_http_queries`（按 `http_query_count` 早退）不会摘除它们；缺失显式摘除时
  停用态 handler 仍在位（业务 gate 拒绝但 HTTP 200），且 activate 重注册会走 duplicate 冲突
  触发回滚把整组槽置墓碑（真实机复现，见 §4）。
- **Kotlin 收窄**：`DebugController` 11 条 POST 改为 `NativeHttpCommand.post` 透传（EB 7 条经
  `postEb` 做 body 校验）；`ExtensionBagActions` 5 方法、`InfoSystemQueries` 的
  settingsUi inject/restore/open-option/open-panel 与 extensionBagTestEquip/TestItem、
  `ActionApiService`/`InfoApiService` 对应声明全部删除。JNI external 签名按冻结约束保留。

## 3. 静态验证

| 命令 | 结果 |
|---|---|
| `git diff --check` | 通过 |
| Host CTest | `100% tests passed out of 65`（基线不变：EB/core.ui 插件源不在 host 测试编译面） |
| `check_http_routes.py` | 143/143、三表一致、owner 不变 |
| `check_arch.py` | 全部通过、无新增基线 |
| `scripts/build-debug.sh` | `2609231318_8fa952a58351` |
| `scripts/build-debug.sh -PqolKernelOnly=true` | `2609231313_7a5736f5c2f2` |
| kernel-only `.so` strings | `extension_bag/enter_view`、`settings-ui/inject` 命中 0；普通构建命中 3 |

## 4. 真机验收（monster v29 集成包）

health `ready=true` → dialog select ok → `enter_slot` 进档 `world`。

| 检查点 | 结果 |
|---|---|
| settings-ui 安全序列 | inject 200 `{"ok":true}`、status 200（真实状态）、restore 200 `{"ok":true}` |
| EB 业务响应 | `enter_view`→`inventory panel not open`；`click_item`/`exit_view`→`not in extension view`；`equip`→`bad virtual bag index or bag type`；`item` 合法参数→200 `{"ok":true}` |
| 非法 body（迁移前 400 口径） | equip/item 空 body → 400 `{"ok":false,"error":"bad request"}`（与基线逐字一致） |
| 校验文案 | `bag:3` → 400 `bag required (6-10)` |
| EB lifecycle | deactivate → 503 `http owner inactive`；activate → 200 `{"ok":true}` |
| core.ui lifecycle | deactivate → 503 `http owner inactive`；activate → 200 `{"ok":true}` |
| 全量 smoke | `total=143 pass=125 warn=18 fail=0`；与基线 diff 仅 5 条已知 op 门控环境差异（party 4 + quit_quest，批 4-1/5-0 同款定性） |
| logcat | FATAL/SIGSEGV/SIGABRT/UnsatisfiedLinkError = 0 |

### 过程发现（已修复）

1. **自注册 handler 停用不摘除**：首次 lifecycle 重放中 EB deactivate 后 `enter_view` 返回 200
   `extension bag disabled`（handler 仍在位），activate 后转为 503 `http owner inactive`
   （重注册 duplicate → 回滚置墓碑）。根因即 §2「显式摘除」缺失；补 `unregister_owner` 后
   两 owner 的 deactivate/activate 全链通过。
2. **`open-option` / `open-panel` 与 `inject`/`restore` 连发会崩游戏**：tombstone 为纯原版
   `libgame.so`（`GRPX_DrawPart ← GAMELOADER_DrawBackGround ← Scene_Draw_POPUP_SC_OPTION_MMENU`），
   无模块帧——settings-ui 调试端点会改写原版选项 popup 的回调槽/生命周期，与批 1 smoke 将这两条
   显式跳过（记 warn）同因。验收与 smoke 均按该口径跳过 open-*；迁移未改变业务函数，仅传输路径变化。
   `inject → restore` 单独成对调用安全（本次已逐步验证 200）。

## 5. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `host/http_dispatch.cpp`、`plugins/extension_bag/plugin.cpp`、`plugins/core.ui/plugin.cpp`、Kotlin `DebugController`/`InfoApiService`/`InfoApiServiceCore`/`InfoSystemQueries`/`ActionApiService`/`ActionApiServiceCore`/`ExtensionBagActions` | 本批无扩展背包 R-xx/B-xx（HTTP 传输与生命周期接线，不改 EB 业务/锁/事务语义） | VM-K5-05b | 全部已执行：host/静态/双构建/strings/LSPatch/真机 11 条 + 双向 lifecycle + smoke + logcat |

## 6. 备注

- EB/core.ui 的 command handler 未进 descriptor，故 `check_http_routes` 的 owner 计数不变、
  kernel-only 符号自然为 0（自注册代码在插件 TU 内）。
- 剩余 51 条 POST（api.character/item/world/save/ui/quest 等）仍待后续批次；本批只动已有真实
  owner 的两组。
