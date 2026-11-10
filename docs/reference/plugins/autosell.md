# `autosell` 对外契约

`autosell` 是按存档规则自动出售背包物品的功能插件。它判定哪些物品符合当前存档的筛选规则，并把出售副作用交给 `core.item` 的 `inventory.action`；插件本身不计算售价、不扣款、不移除物品。

本文与 [`autosell-internals.md`](autosell-internals.md) 是 `autosell` 的仅有两份权威文档。`docs/archive/` 与 `docs/refactor/audits/` 下的自动出售材料仅作历史追溯，不作为当前契约或实现依据。

本文只描述调用方可以依赖的行为。目录、资源 claim、扫描实现和持久化细节见 [`autosell-internals.md`](autosell-internals.md)。

## 职责边界

| `autosell` 负责 | 不负责 |
|---|---|
| 自动出售规则：品质、强化耐久度、总孔数、宝石档位、宝石属性，以及背包/徽章/骰子的出售范围。 | 售价、扣款、物品移除与回滚；由 `core.item` 的 `inventory.action` 承担。 |
| 按存档槽保存和读取筛选配置，并向 HTTP/UI 提供当前状态。 | 物品事实与游戏内存读取；由 `item.facts`、Game Host 提供。 |
| 扫描节拍、扫描门禁、轮次取消和出售统计。 | 宝石属性百分位计算；由 `attribute.quality` 提供。 |
| 背包页齿轮入口与游戏内配置面板。 | 存档文件 IO、slot-data 会话与保存事务；由 `core.save` 提供。 |
| 规则判定与背包/徽章/骰子分档识别。 | 扩展袋数据与存储；由 `core.item` 的袋源机制和扩展袋 owner 承担。 |

## 术语与身份

| 名称 | 含义 |
|---|---|
| `slot` | 存档槽 `0..2`。配置按槽保存；切换存档不会把上一个槽的配置带到新槽。 |
| 物品身份 | 出售时使用 `core.item` 的 `QolItemHandle {slot, generation}` 与 `QolActionRef {space_id, bag, slot}`；`autosell` 不定义新的物品编号。 |
| 扫描范围 | 原版物理袋 `space=0` 的 `bag 0..4`、C18 原生追加袋 `space=1`，以及 `inventory.bag_source` 注册源 `space>=2`。任务袋 `5` 不处理。 |
| 规则值 | 数值规则 `0` 表示关闭；`rarity`/`gemTier`/`gemRange` 的正整数 `v` 表示阈值 `v-1`，比较方向 `<=`；`backpackTier`/`badgeTier`/`diceTier` 的正整数 `v` 直接表示最大可售档位。 |

物品句柄只在进程内有效，不能持久化；扫描和出售都要求句柄仍然有效，失效时静默跳过该物品。

## 能力与开关

`autosell` 不提供 capability，也没有其他插件依赖它。它消费以下能力（声明见 `plugins/autosell/plugin.cpp`）：

| Capability | 版本 | 用途 |
|---|---:|---|
| `attribute.quality` | 2.0 | 读取宝石属性百分位，用于宝石属性规则。 |
| `inventory.action` | 3.0 | 执行出售；这是插件唯一的物品副作用入口。 |
| `world.facts` | 2.0 | 判断世界状态与游戏状态门禁。 |
| `item.facts` | 3.6 | 读取物品分类、真实装备/宝石属性、禁售标记和物品视图。 |
| `save.slot_data` | 1.0 | 按存档槽读写 `autosell` sidecar 配置。 |
| `ui.state` | 1.2 | 判断背包页/对话框是否占用扫描。 |
| `inventory.bag_source` | 1.2（可选） | 枚举已注册扩展袋源并读取其中的物品；缺失时只跳过注册源。 |

可选依赖：`inventory.bag_source` 1.2、`http.registry` 1.1（HTTP 路由元数据）、`ui_panels` 1.3（打开/关闭游戏内面板）。缺失可选依赖不影响原版物理袋和 C18 追加袋扫描；任一必需能力不可用时，Host 会把插件判为 `blocked`。`extension_bag` 不是本插件的依赖；扩展袋数据经 `core.item` 的 `inventory.bag_source` 访问，存储仍归扩展袋 owner。

| Feature | 默认 | 类型 | 行为 |
|---|---|---|---|
| `autosell.auto_sell` | 关闭 | 游戏玩法 toggle | 开启后插件安装并启动扫描；关闭后停止扫描、隐藏入口并请求关闭面板。 |

feature 定义见 `plugins/autosell/plugin.cpp`。该 feature 是插件唯一用户开关，不额外提供“立即扫描”或“全局启用”开关。

## 公共契约

`autosell` 没有跨插件 C++ capability，也没有 `provides`。其他插件不应直接调用其内部函数或依赖其实现头；可交互的公开面只有：

1. feature `autosell.auto_sell`；
2. HTTP 配置接口；
3. 游戏内背包页入口和配置面板。

HTTP 与游戏内面板共用同一条配置提交路径，写成功后的运行时配置会立即影响下一次扫描。

## 行为矩阵

规则实现见 `plugins/autosell/rules.cpp`；面板档位与标签见 `plugins/autosell/ui/panel_events.cpp` 和 `plugins/autosell/ui/panel_internal.h`。

### 装备规则

三条装备规则只对 **`item.facts.item_type == kEquipment`** 生效（不是库存视图里的“不可堆叠”字段）。**品质为主规则，宝石孔与强化耐久度为副规则**：

- `rarity` 开启时，设阈值 `T = rarity-1`（配置允许出售的最高品质档）：
  - 品质 `< T`：直接出售，**无视** `enhance` / `socket`；
  - 品质 `== T`：所有已开启的副规则必须命中才出售；副规则全部关闭时直接出售；
  - 品质 `> T` 或品质未知：不出售。
- `rarity` 关闭时，副规则退化为纯 **AND** 过滤：至少开启一条，且所有已开启项都命中才出售。

| 字段 | 档位 | 出售条件 |
|---|---|---|
| `rarity` | 1..5 → ≤白/≤绿/≤蓝/≤黄/≤紫 | 品质 `<= rarity-1`；低于阈值直接出售，等于阈值受副规则约束。品质未知时不出售。 |
| `enhance` | 1..32 → ≤0..≤31 | 剩余强化耐久度 `<= enhance-1`（仅作为阈值档的副规则或 `rarity` 关闭时的 AND 项）。 |
| `socket` | 1..16 → ≤0..≤15 | 总孔数 `<= socket-1`（同上）。 |

示例：`rarity=3`（≤蓝）时，白/绿装备直接出售；蓝装备仅在 `socket` 与 `enhance`（已开启者）同时满足时出售；黄/紫永不出售。

### 宝石规则

两条宝石规则只对宝石生效，同类内为 **OR**：任一已开启规则命中即出售。

| 字段 | 档位 | 出售条件 |
|---|---|---|
| `gemTier` | 1..5 → ≤低级/≤中级/≤高级/≤顶级/≤混沌 | 宝石档位 `<= gemTier-1`。 |
| `gemRange` | 1..5 → ≤30%/≤60%/≤75%/≤90%/≤99% | 属性百分位 `<= 30/60/75/90/99`。百分位未知时不命中。 |

最高档 `gemRange=5` 的阈值是 99，因此满分（100）宝石不会被该档出售。

### 背包、徽章、骰子分档

三项都是“最大可售档位”，`0` 为关闭，正整数表示出售该档及更低档：

| 字段 | 档位 | 出售条件 |
|---|---|---|
| `backpackTier` | 0..4 → 关/≤手提包/≤背包小/≤背包中/≤背包大 | `item_type == kBackpack`，且 category `1..backpackTier`。 |
| `badgeTier` | 0..4 → 关/≤普通/≤被祝福/≤教团/≤英雄 | `item_type == kMercenarySeal`，rank `1..badgeTier`；普通 42..47、被祝福 48、教团 50、英雄 49 与 928..933。教团排在英雄前。 |
| `diceTier` | 0..5 → 关/≤低级/≤中级/≤高级/≤顶级/≤混沌 | `item_type == kDice`，tier `1..diceTier`；category 52..56 对应 1..5。混沌骰子仅在 `diceTier=5` 时出售。 |

旧配置的 `special` 数组仍兼容：`backpack`→`backpackTier=4`、`normalSeal`→`badgeTier=1`、`dice`→`diceTier=4`（旧普通骰子不含混沌）。显式 tier 字段优先于 `special`。

### 组合与保护

| 规则 | 语义 |
|---|---|
| 装备组 / 宝石组 / 背包徽章骰子组之间 | **OR**：任一组命中即出售。 |
| 硬保护 | 已强化或已镶嵌的**真实装备**永不出售，优先于所有规则；非装备的原始强化/孔位字段不参与保护。 |
| `enabled=false` | 所有规则均不生效。 |
| 规则全为 0 | 不出售。 |
| 未知字段值 | 需要该字段的规则不命中（fail-closed），不会按 0 猜测。 |

### 游戏内面板

- 开启 feature 后，背包/装备页出现齿轮入口；点击打开「自动出售」面板。
- 面板编辑的是本地草稿；点击左上返回或由宿主关闭面板时提交，编辑过程中不即时改变扫描配置。提交后会重置一次扫描计时器，下一次扫描从关闭时刻重新等待 20 帧，避免关闭瞬间抢在背包页 active 之前出售。
- 面板中的右上 √/X 切换本存档的 `enabled`；8 行规则分别调整品质、耐久度、总孔数、宝石等级、宝石属性、骰子、背包、徽章。
- 右列 5 行（宝石两项 + 骰子/背包/徽章）与宝石同列，不再有独立的宝石分组框；未启用文字为灰色。
- 面板打开期间扫描被阻塞，避免草稿编辑与后台出售并发。
- 面板打开时 `/api/ui/state` 报告 `module_panel_autosell`（`type=5`，`source=2`）。

## HTTP surface

两条路由由 Kotlin controller 处理，不需要 OP 权限。详细请求/响应示例与冻结字段以 [`reference/api-reference.md`](../api-reference.md) 的「自动出售配置」小节为准；实现见 `AutoSellFeatureController.kt`。

| 方法 | 路径 | 用途 | 门控 |
|---|---|---|---|
| `GET` | `/api/feature/autosell/config` | 读取当前存档配置与运行状态。 | 插件需处于可用态；无活动存档时返回当前状态中的 `slot=-1`/`loadedSlot=-1`。 |
| `POST` | `/api/feature/autosell/config` | 写入当前存档配置并持久化。 | 需要活动且已加载的存档会话；无会话返回 409。 |

POST 字段：

| 字段 | 类型 | 范围/取值 | 缺省 |
|---|---|---|---|
| `enabled` | boolean | `true`/`false` | `false` |
| `rarity` | integer | 0..5 | `0` |
| `enhance` | integer | 0..32 | `0` |
| `socket` | integer | 0..16 | `0` |
| `gemTier` | integer | 0..5 | `0` |
| `gemRange` | integer | 0..5 | `0` |
| `backpackTier` | integer | 0..4 | `0` |
| `badgeTier` | integer | 0..4 | `0` |
| `diceTier` | integer | 0..5 | `0` |
| `special` | string array | `backpack` / `normalSeal` / `dice`（旧兼容；tier 字段优先） | `[]` |

`GET` 返回的 `special` 按 `backpack`、`normalSeal`、`dice` 固定顺序，由 tier 字段派生；`POST` 接受任意顺序，重复元素按集合处理。新调用方应使用 `backpackTier`/`badgeTier`/`diceTier`。

成功响应与 GET 相同，返回写入后的状态 JSON，不使用 `ok` 包裹。无活动存档会话时拒绝写入且不修改运行时配置；持久化请求失败时运行时配置仍会生效，dirty 保留并由后台重试补发，`persisted` 保持 `false` 直到写入确认。

## 状态与错误

状态 JSON 字段（来源见 `plugins/autosell/status_json.cpp`、`plugins/autosell/config.cpp`）：

| 字段 | 含义 |
|---|---|
| `enabled` / `rarity` / `enhance` / `socket` / `gemTier` / `gemRange` / `backpackTier` / `badgeTier` / `diceTier` / `special` | 当前运行时配置；`special` 是兼容派生字段。 |
| `sold` / `failed` | 当前进程内累计出售事务成功/失败次数；不按存档槽清零。 |
| `lastScanFrame` | 最近一次实际执行扫描的帧号；`-1` 表示尚未扫描。 |
| `slot` / `loadedSlot` | 当前 slot-data 会话槽与已加载槽；无会话时为 `-1`。 |
| `persisted` | 最近一次配置写入是否已确认落盘。POST 返回 `false` 只表示写入尚未完成，可轮询 GET 确认。 |
| `hostInstalled` | 主线程扫描宿主是否已安装。 |

| 情况 | HTTP 结果 |
|---|---|
| JSON 非法 | `400 bad request` |
| 字段类型/范围非法，或 `special` 元素非法 | `400`，错误信息指出字段 |
| 当前无活动存档会话，或写入未被接受 | `409 config rejected (no active save session)` |
| 插件未启用/已停用 | 路由 owner 不可用（通常为 `503 http owner inactive`） |

扫描期间单件出售失败不会中断整轮扫描；`stale`、`empty_slot`、`no_item_handle` 视为物品已变化并静默跳过，其他失败计入 `failed` 并记录 WARN。

## 故障排查

- **面板/背包打开时不会扫描**：`inventory` 面板或自动出售面板 active 时，`ui_busy`/panel gate 会暂停本轮；关闭后最多等待 20 帧。
- **`lastScanFrame` 不更新**：看 INFO 日志 `scan gate ... reason=...`，按 `global_disabled`、`session_inactive`、`config_disabled`、`not_in_world`、`game_state_blocked`、`ui_busy` 逐项排除。
- **`lastScanFrame` 更新但 `sold=0`**：确认目标 tier 不是 `0`（0=关）；查看 WARN `scan item view skipped reason=...` 和 `sell failed ... status=...`。
- **扩展袋**：C18 原生追加袋是 `space=1`；`inventory.bag_source` 注册源是 `space>=2`。任务袋 `space=0 bag=5` 不扫描。

## 线程与不变量

- 扫描只在游戏主线程执行，由帧调度器每 20 帧触发一次；扫描不持有插件私有锁跨插件调用，也不主动保存存档（`plugins/autosell/scan.cpp`、`plugins/autosell/scan.h`）。
- 扫描覆盖原版物理袋 `0..4` 和 `inventory.bag_source` 注册源；注册源通过 `source_count`/`source_at` 枚举，未注册任何源时物理袋照常扫描。
- 配置读写允许来自 HTTP/JVM 线程和游戏主线程；运行时配置快照是线程安全的。面板与 HTTP 的写入共用同一条提交路径。
- 只有活动且已加载的存档会话才能提交配置；`save_exit` 后写入会被拒绝，直到下一次成功加载。
- 扫描只在世界内、游戏状态允许、背包页/自动出售面板未占用时运行；关闭 feature、关闭存档配置或退出存档都会取消当前轮次。已进入 `inventory.action.sell` 的单笔事务允许完成。
- `autosell` 不直接访问游戏内存、不计算售价、不执行物品移除；这些行为由 `core.item`、Game Host 和 `attribute.quality` 承担。

实现细节见 [`autosell-internals.md`](autosell-internals.md)。
