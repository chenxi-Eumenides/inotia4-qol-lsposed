# 日志审计清单：批次 4（http / api / op）

依据 `docs/reference/logging-standards.md` 与 `docs/reference/logging.md`。

## 范围说明

本批次原定覆盖 `http`、`api`、`op` 三个域。实际分布：

| 侧 | 域 | 生产日志 |
|---|---|---:|
| Native | `api` | 1 条（`bridge/native/gamebridge_internal.h` 的 `MOVE_LOG`，`op_result` 调用） |
| Native | `http` | 0 条 |
| Native | `op` | 0 条 |
| Kotlin | `op`/`http`/`api` | 21 条（`OpController.kt`、`SaveActions.kt`、`UiActions.kt`、`ConfigApiService.kt`） |

Kotlin 日志按本轮约定（`logging-standards.md` §10）不在改造范围；HTTP 服务的启动/停止/错误日志均在 Kotlin 侧。Native 侧 `http` 域只有域 token 定义，没有调用点。

因此本批次可审议的 Native 日志为 1 条。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| `bridge/native/gamebridge_internal.h:35` | api | I | `[op] args={...} -> result`；所有 JNI op 结果经 `op_result` 输出 | API 请求完成 | I | augment | 每次外部 op 调用记录一次，频次有限，I 正确；正文缺结构化键，改为 `op=%s args={%s} result=%s`，避免 `[op]` 与箭头符号难以解析 |

## 缺失日志建议

无。Native `api` 域的请求结果已由 `op_result` 覆盖；HTTP 服务生命周期由 Kotlin `LogFile` 记录（本轮不改造）。

## 统计

- Native 生产日志：1 条
- disposition：keep 0，augment 1
- Kotlin 跳过：21 条
- 补充建议：0

## 审核裁决

- `gamebridge_internal.h:35`：执行 `augment`，等级保持 I。
- 不新增 Native `http`/`op` 域日志：这些事件在 Kotlin 侧已有 `LogFile.op` 记录，重复新增会破坏单一事实来源。
- 不新增 `op_result` 的 DEBUG 参数明细：`args` 已是有限字段，`result` 为最终结果，无需再分级。

## Top 发现

1. Native `http`/`op` 域没有生产调用点，日志实际来源是 Kotlin 的 `LogFile.op` 与统一 facade。
2. 唯一 Native API 日志正文是 `[op] args={} -> result` 自然格式，已改为 `op= args= result=` 键值形式。
3. Kotlin 侧 21 条 op 日志的等级与内容审计，待 Kotlin 解冻后单独执行。
