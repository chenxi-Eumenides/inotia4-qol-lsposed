# 日志记录规范（编写与审计契约）

本文是**日志内容、等级选择、频率与去重**的当前权威，面向两类读者：

- 写日志的开发者/代理：按本文决定一条日志该不该写、写哪一级、正文写什么。
- 审计日志的代理：按本文的判定问句和处置词表逐条审议，结论可复现。

行格式、文件路径、轮转、崩溃取证见 [`logging.md`](logging.md)。两者冲突时：**格式以 `logging.md` 为准，等级语义与正文内容以本文为准**。

本文描述当前日志契约；仍未实施项集中在 §10。

---

## 1. 四级语义

等级由「失败后果」决定，不由代码位置或异常类型决定。

| 等级 | 含义 | 必须满足 | 典型事件 |
|---|---|---|---|
| `E` error | 出错了，模块或进程无法继续提供该能力，或数据损坏不可自愈 | 用户/调用方无法通过继续操作绕过；需要人工处理或重启 | native_init 被拒、bridge/JNI 注册失败、crash handler 安装失败、存档文件损坏、重试耗尽后的最终失败 |
| `W` warn | 做了但失败了，游戏还能继续运行 | 本次操作失败，但已降级或可重试；系统状态仍一致 | hook 安装失败、方法/字段查找失败、游戏结构不匹配、单次调用返回失败、非法输入被拒、中间重试失败 |
| `I` info | 记录做了什么 | 生命周期或状态确实发生了变化；一次会话中出现次数有限 | 模块加载、环境指纹、hook 安装成功、功能启停、存档/切图、面板开关、API 请求完成 |
| `D` debug | 为定位 bug 获取信息 | 只在排查时需要；默认关闭 | hook 入参/返回值、状态机迁移、分支选择、逐帧数值、对象身份与数量 |

判定顺序（从高到低，命中即定级，不再往下看）：

1. 能力整体不可用或数据不可恢复 → `E`
2. 单次操作失败但游戏继续 → `W`
3. 状态确实改变且需要回放 → `I`
4. 只为解释「为什么」→ `D`

补充约束：

- **Kotlin `LogFile.error` 会额外触发用户可见弹窗**（`ModuleErrorDialog`，队列上限 8 条）。因此 Kotlin 侧 `E` 额外要求「用户必须知道且需要采取行动」；可降级的失败一律 `W`。本轮 Kotlin 日志不纳入改造范围，但新增 Kotlin 日志必须遵守本条。
- 同一失败只能有一个最终等级。不得同一事件先 `E` 再 `W`，或上下层各记一次。
- 异常对象只用于补充原因，不改变等级判定。

---

## 2. 重试与多次失败

重试过程中的每一次失败都要可区分，最后一次失败才升级为 `E`。

| 时机 | 等级 | 必填字段 |
|---|---|---|
| 中间第 i 次失败（还会继续重试） | `W` | `op`、`attempt=<i>/<n>`、`reason=<原因>` |
| 重试耗尽/放弃重试 | `E` | `op`、`attempts=<n>`、`reason=<原因>` |
| 失败后切换到降级路径且成功 | `I` | `op`、`fallback=<替代路径>` |

禁止只写 `retry failed`、`operation failed` 这类无法区分次数和原因的文案。每次 `W` 的 `attempt` 与 `reason` 必须不同，否则视为重复日志。

示例（`src:line` 由 logger 填充，此处省略）：

```text
2026-11-12 20:14:03.118 f=1842 W save save.cpp:214 op=save_slot attempt=1/3 slot=2 reason=store_busy
2026-11-12 20:14:03.181 f=1843 W save save.cpp:214 op=save_slot attempt=2/3 slot=2 reason=store_busy
2026-11-12 20:14:03.244 f=1844 E save save.cpp:214 op=save_slot attempts=3 slot=2 reason=store_busy
2026-11-12 20:14:03.245 f=1844 I save save.cpp:232 op=save_slot fallback=queue_to_disk slot=2
```

单次操作、无重试的失败：直接 `W`（可继续）或 `E`（不可继续），不要为了凑 `attempt` 造字段。

---

## 3. 正文结构

统一形式：

```text
<verb> <key>=<value> [<key>=<value> ...]
```

- `verb`：英文小写动词，固定词表：`init`、`start`、`stop`、`install`、`uninstall`、`skip`、`reject`、`fail`、`fallback`、`retry`、`apply`、`bind`、`save`、`load`、`drop`、`move`、`open`、`close`、`dispatch`、`restore`、`degrade`。
- 键：英文小写，单词间用 `_`。值：数字、枚举 token、对象 id；不写自然语言句子。
- 头部已有时间、帧号、等级、域、`src:line`，正文不得重复这些信息。
- 正文不写文件路径、不写「进入某函数」、不写无变量的状态描述。

一条日志至少包含四项信息；缺项即视为无效日志：

1. 动作（`verb`）
2. 操作对象或作用域（如 `slot=2`、`bag=3`、`item=100231`、`hook=item_use`）
3. 关键输入（如 `count=3`、`panel=inventory`）
4. 结果或原因（成功可省略；失败必须有 `reason`）

字段命名引用既有域内约定（`slot`、`bag`、`item`、`hook`、`op`、`attempt`、`attempts`、`reason`、`errno`），不要为同一概念另造同义键。

禁止：

- 多行正文（非 DEBUG 已由 `qol_log_format` 转义）。
- 敏感信息、设备唯一标识、用户数据原文。
- 在正文里拼接时间戳、域名、文件名或等级。
- 单行超过 4096 字节（超长由 logger 截断并补 `...`，调用方不应主动构造超长文本）。

---

## 4. 频率与去重责任

责任划分：**调用方保证不产生无意义的高频日志；logger 只提供兜底去重**。

### 4.1 调用方必须遵守

1. 逐帧路径禁止 `I`。需要逐帧观测的一律 `D`，且不得为它提前构造昂贵数据。
2. 同一事实只在一个调用点记录。上层不得重复记录下层已经记录过的失败。
3. `W`/`E` 不得放在循环体内无区分地重复输出；循环内失败必须带对象 id 或 `attempt`。
4. 可选路径的正常缺失（对象不存在、玩家未进入世界、可选资源缺失）不是失败，用 `D`，不要用 `W`。
5. `I` 只用于本次会话中次数有限的状态变更，不得用于「每次调用都会发生」的热路径。

### 4.2 logger 内部兜底

以调用点 `src:line` 为身份键，不要求调用方声明任何状态：

| 等级 | 内部行为 |
|---|---|
| `D` | 正文未变化不输出；变化且距上次 ≥ 250ms 才输出；同一调用点每帧最多 1 行 |
| `I` | 不门控、不去重、不限流 |
| `W` | 同一调用点、完全相同的正文在 5s 内只输出一次；不同正文（含不同 `attempt`/`reason`）全部保留 |
| `E` | 同一调用点、完全相同的正文在 30s 内只输出一次；同一调用点每 60s 最多 10 行，超出时输出一行 `suppressed` 汇总 |

内部去重只吸收「完全相同的重复」，不替调用方决定业务频率。

---

## 5. 事件类别速查

| 事件 | 等级 | 说明 |
|---|---|---|
| 模块/插件加载、环境指纹、版本信息 | `I` | 每次启动一次 |
| 功能启用/停用、配置生效 | `I` | 带 `feature=`、`section=` |
| hook 安装成功 | `I` | 带 `hook=` |
| hook 安装失败、方法/字段查找失败 | `W` | 该功能不可用，游戏继续；带 `hook=`/`symbol=` |
| 游戏结构与预期不符（descriptor、字段偏移、签名） | `W`；影响所有功能时 `E` | 带期望值与实际值 |
| JNI/bridge 注册失败、native init 被拒 | `E` | 模块能力不可用 |
| 存档写入/读取失败（单次） | `W` | 带 `slot=`、`reason=` |
| 存档损坏、无法恢复、重试耗尽 | `E` | 用户需要处理 |
| HTTP/API 请求完成 | `I` | 由 `LogFile.op` 产生，带 `endpoint=`、`result=`、`dur=` |
| API 请求参数非法或被拒 | `W` | 带 `endpoint=`、`reason=` |
| 用户触发的操作（点击、拖拽、开关） | `I` 仅在状态变化时；过程细节 `D` | 不得每次点击都 `I` |
| 降级路径生效 | `I` | 带 `fallback=` |
| 资源/对象缺失且属可选 | `D` | 不是失败 |
| 状态机迁移、数值变化、返回值 | `D` | 必须受 debug 开关门控 |
| 限流丢弃、封顶标记 | `D` | 由 logger 产生，调用方不手写 |

---

## 6. DEBUG 零成本契约

未开启 `debug.logging` 时，debug 路径不得产生格式化、分配、遍历或哈希表操作。

1. 等级判断必须先于参数求值。native 侧由 `QOL_LOG_DEBUG` 宏保证；Kotlin 侧现状不满足（lambda 无条件求值），本轮不动，见 §10。
2. debug 参数只允许基本类型和 O(1) 表达式。需要序列化、遍历容器、查找名称、拼接字符串时，必须显式短路：

```cpp
if (QOL_LOG_DEBUG_ENABLED()) {
    QOL_LOG_DEBUG(QolDomain::kInventory, "hook=%s raw=%s", name, dump_items().c_str());
}
```

3. 审计时把「debug 参数内存在分配/遍历/序列化」列为 `augment` 或 `delete` 的独立 finding，不因为「反正是 debug」放过。
4. 可选字段的构造（例如为日志计算 size、复制数组）同样受本条约束。

---

## 7. 审计流程

### 7.1 逐条记录格式

每条日志一行，固定 8 列：

```text
file:line | domain | level_now | message | event_class | level_target | disposition | rationale
```

`disposition` 只用六个值：

| 处置 | 含义 |
|---|---|
| `keep` | 等级、内容、频率均符合，不改 |
| `demote` | 降级（如 `E`→`W`、`W`→`D`） |
| `promote` | 升级（如 `W`→`E`） |
| `delete` | 无信息量、重复事实或热路径噪声，删除 |
| `merge` | 多条记录同一事实，合并到一个调用点 |
| `augment` | 保留但补字段、拆分 `attempt`、改为结构化正文 |

### 7.2 每条必答的判定问句

1. 这条日志对应的事件，失败后游戏还能继续吗？能则不得为 `E`。
2. 它提供了哪些字段？去掉域名和文件名后，是否仍能回答「谁、做了什么、对什么、结果」？
3. 同一事实是否在别处已经记录过？有则 `merge` 或 `delete`。
4. 它会在循环或逐帧路径中执行吗？是则不得为 `I`，且 `W`/`E` 必须带区分字段。
5. `W`/`E` 的正文是否包含原因？无 `reason` 的失败日志必须补。
6. debug 参数是否包含分配、遍历或序列化？有则 `augment` 为显式短路。
7. 文案是否混用中英文、是否重复头部信息？有则改写。

### 7.3 批次

| 批次 | 范围 | 约计 |
|---|---|---|
| 0 | logger 内部机制（§4.2、§6、§10） | 基础设施 |
| 1 | platform + core + infra/log + patch + bridge | 200 |
| 2 | extension_bag + core.item + core.inventory | 250 |
| 3 | save + save_backup + autosell | 90 |
| 4 | http + api + op | 40 |
| 5 | craft + custom_recipe + attr_range + world_tuning + special_equip + character_panel + world_teleport | 120 |
| 6 | ui + config + catalog + settings + core.custom_ui | 90 |

每批产出 `inventory.md`（逐条清单）与 `changes.md`（改动汇总），放在 `docs/refactor/audits/log-audit-20261112/`。

---

## 8. 验收指标

1. 默认（debug 关闭）冷启动 + 10 分钟游玩：`qol.log` ≤ 512 KiB，`E` 行 ≤ 2，`W` 行 ≤ 30。
2. debug 关闭时 debug 路径无格式化、无哈希表读写（计数器或基准断言）。
3. debug 开启连续 30 分钟不触发 4 MiB 轮转。
4. 同一调用点在 10 分钟日志内不出现内容完全相同的连续行。
5. 任意 `W`/`E` 行都能回答：什么操作、什么输入、失败原因、是否降级。
6. 逐帧路径内 `I` 行数为 0（静态审计）。

---

## 9. 等级与域的一致性

- 域 token 与 `QolDomain` 枚举一致，新增域必须同时更新 `qol_log.h`、`qol_log.cpp` 的 `kDomainTable` 和 Kotlin `LogDomain`。
- 不得为绕过等级规范新增域或新增等级。
- 日志等级不得运行时可变；debug 只有开/关两态。

---

## 10. 尚未实施项

| 项 | 现状 | 目标 |
|---|---|---|
| Kotlin debug lambda | 参数 lambda 无条件求值 | 关闭 debug 时避免 lambda 参数求值 |
| `LogFile.isDebugEnabled()` | 恒返回 `true` | 返回实际 debug 开关状态 |

---

## 变更记录

| 日期 | 内容 |
|---|---|
| 2026-11-12 | 建立规范：四级语义、重试分级、正文结构、去重责任、审计流程、验收指标 |
