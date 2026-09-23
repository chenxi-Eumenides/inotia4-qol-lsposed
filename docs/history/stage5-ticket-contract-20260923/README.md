# 阶段 5 票据结果契约：infra 结果通道（VM-K5-06a）

日期：2026-09-23 ｜ 前置：用户裁决 A（写路由转票据 + 结果回取），并要求先改 ARCHITECTURE 规则。

## 1. 交付内容

### 文档（先于实现，按用户指定顺序）
- `ARCHITECTURE.md`：修订「成功响应冻结」规则（§2.2 红线、§7.1、§8.1）——
  query 与未迁移写路由响应冻结；带游戏内存写入的写路由 POST 返回票据，原结果经票据终态
  尽力回取；受影响路由须在 api-reference 逐条公布。
- `docs/reference/api-reference.md`：§7.9 票据端点扩为结果回取端点（state 六态、result、
  result_retained、尽力保留窗口）；新增 §7.10 写端点票据契约与调用方迁移指引。
- `docs/refactor/current-optimization-review.md` §8 行 5：阶段 5 验收条件同步为票据契约口径。

### 实现（公共 ABI 按用户修正）
- `contracts/commands.h`：状态常量（pending/done/failed/cancelled/**unknown/expired**）、
  `QolTypedResultCommandHandler`、`QolTicketResultApi`（注册/注销结果处理器、读结果、细化状态）。
- `contracts/host_sdk.h`：结果面追加在 **QolHostSdk 真尾部**（不动嵌在中段的 `QolCommands`），
  `kHostSdkMinor` 7→8，旧插件按 `abi.size` 消费前缀。
- **旧 handler 签名保留**：`QolTypedCommandHandler` 不变；另增可写结果的变体
  `QolTypedResultCommandHandler`（同 id 空间，互斥注册）。
- `infra/command_queue.{h,cpp}`：`Command.run_with_result`（旧 `run` 保留）；终态携带结果文本；
  单结果 16 KB、总预算 512 KB、最旧优先淘汰结果（保留状态）；`ticket_state` 区分
  unknown/expired/failed；新增 `read_result`。
- `infra/commands.{h,cpp}`：结果处理器注册/注销、`commands_read_result`、`commands_ticket_state`；
  typed 命令统一经 `command_run_with_result`（无结果处理器时退回纯执行）。
- `host/host_sdk.cpp`：装配尾部结果面；`host/status_json.cpp`：票据 JSON 输出六态 + result +
  result_retained（非 JSON 结果不内嵌）。

## 2. 验证

| 项 | 结果 |
|---|---|
| Host CTest | 65/65 通过（新增 `test_typed_result_commands`：结果回取、无结果失败、unknown/expired 区分） |
| 语义变更测试更新 | `test_infra_command_queue`/`test_infra_config_commands` 的「未知票据=failed」断言改为 unknown/expired |
| `git diff --check` / `check_arch` / `check_http_routes` | 通过（143/143） |
| 构建 | 普通 `2609231658_c06b36657ba4`、kernel-only `2609231658_2612321c82d8` |
| LSPatch | 3/3（monster v29 已安装并核验嵌入模块） |
| 真机 | `tickets/0`→`{"ok":false,"error":"invalid ticket"}`；`tickets/999999`→`state:"unknown"`,`result_retained:false`；lifecycle 票据→`state:"failed"`+`result_retained:false`（动作被拒，语义正确）；FATAL/SIGSEGV=0 |

## 3. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `ARCHITECTURE.md`、`docs/reference/api-reference.md`、`docs/refactor/current-optimization-review.md`、`contracts/commands.h`、`contracts/host_sdk.h`、`infra/command_queue.{h,cpp}`、`infra/commands.{h,cpp}`、`host/host_sdk.cpp`、`host/status_json.cpp`、`tests/test_infra_{command_queue,config_commands}.cpp` | 无扩展背包 R-xx/B-xx | `VM-K5-06a` | 全部已执行：host/静态/双构建/LSPatch/真机票据端点；逐路由迁移与 EB 回补在后续批次 |

## 4. 遗留（后续批次）

- 逐域写路由迁移（character 20 / item 13 / world 5 / ui 5 / save 3 / quest 1 = 47）：
  每批注册结果处理器、Kotlin 收窄为 `NativeHttpCommand.post`、api-reference §7.10 追加、smoke 基线更新、VM 真机。
- 批 5-1 EB 7 条回补为主线程 owner command（用户明确要求）。
- 12 条 501 占位路由：文档标注未实现，不纳入迁移。
