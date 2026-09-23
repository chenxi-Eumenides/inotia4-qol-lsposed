# 阶段 5 批 5-2：EB 7 条写路由回补为主线程 owner command（VM-K5-06b）

日期：2026-09-23 ｜ 用户明确要求：把批 5-1 的 EB 写路由一并回补为主线程 owner command，并逐路由验证失败语义、API smoke 与受影响 VM 真机日志。

## 1. 改动

`plugins/extension_bag/plugin.cpp`：
- 新增 `EbCommandPayload{kind,a,b,c,d}`（20 B ≤ 128 B typed payload 上限）与全局 result handler id
  `0x45420001`（typed handler id 为全局空间，EB 前缀避免与 quest `0x51535401`、custom_recipe 冲突）。
- 新增 `eb_result_handler`（`QolTypedResultCommandHandler`）：**主线程**执行既有 `data_op_extension_bag_*`
  / `data_virtual_bag_test_*`，把原响应 JSON 写入票据结果缓冲；业务失败（原响应含 `"ok":false`）
  返回 false → 票据 `state=failed` 且结果仍携带原错误体。
- 改写 `eb_command_handler`（HTTP 线程）：**仅**做 body/字段校验（400 语义与迁移前逐字一致）→
  构造 payload → `sdk->commands.post_typed(kEbResultHandlerId, ...)` → 写 `{"ok":true,"ticket":N}`；
  投递失败 → 503 `command port/queue unavailable`。**不再直接调用游戏写**。
- `register_extension_bag_commands`：先按 abi 协商（`abi.size`/`abi.minor>=8`）注册结果处理器，
  再注册 7 条 http command；任一步失败回滚两者。
- `plugin_deactivate`：显式注销结果处理器（与既有 `unregister_owner` 同理，自注册不在 descriptor）。

Kotlin 侧无需改动（`DebugController` 已是 `NativeHttpCommand.post` 透传；响应变为票据由客户端按 §7.10 轮询）。

## 2. 验证

| 项 | 结果 |
|---|---|
| Host CTest | 65/65（EB 插件源不在 host 编译面，同批 5-1） |
| 构建 | 普通 `2609231701_cda55fa98dd9`；kernel-only 由 EB 排除不受影响 |
| LSPatch | 3/3（monster v29 已安装） |
| 真机·校验前置 | `enter_view{bag:3}` → 400 `bag required (6-10)`；`click_item{bag:6,slot:99}` → 400 `slot required (0-15)`（均**未**产生票据） |
| 真机·票据 | `enter_view{bag:6}` → 200 `{"ok":true,"ticket":9}`；轮询 `/api/system/tickets/9` → `state:"failed"`、`result:{"ok":false,"error":"inventory panel not open"}`、`result_retained:true`（业务失败时结果仍携带原错误体） |
| 真机·失败语义 | 非 world 时 `result:{"ok":false,"error":"not in game"}`（原响应原样回取） |
| 真机·生命周期 | `extension_bag` deactivate → 6 条合法请求全 503 `{"ok":false,"error":"http owner inactive"}`；activate → 全部 200 恢复 |
| API smoke | `total=143 pass=124 warn=19 fail=0`；与基线 verdict/status diff 仅 4 条已知 party op 环境差异（**零新增**——smoke 对写路由发故意非法 body，400 校验前置，故基线无需变更） |
| logcat | FATAL/SIGSEGV/SIGABRT = 0 |

## 3. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `plugins/extension_bag/plugin.cpp`、`docs/reference/api-reference.md`（§7.10 登记 EB 7 条） | 无扩展背包 R-xx/B-xx | `VM-K5-06b` | 全部已执行：host/构建/LSPatch/真机（校验前置、票据、结果回取、失败语义、生命周期）/smoke/logcat |

## 4. 遗留

- 成功路径的 EB 业务结果（如真实进入扩展背包视图）需游戏内 UI 配合，属 `VM-D2-01..04` 深度行为卡（待人工配合）；本批已验证非成功路径与语义完整。
- 后续批次：47 条真实写逐域迁移（quest 1 / save 3 / world 5 / ui 5 / item 13 / character 20）。
