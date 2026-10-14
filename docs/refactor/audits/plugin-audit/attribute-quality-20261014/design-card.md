# `attribute_quality` v2.0 设计与验收卡

> 本卡描述当前已落地的代码契约及其验收边界，不是未来方案。Oracle 已完成本范围静态复核：R01/R02 修复确认，无新增阻断；契约/feature 注释与 success-code 观察均确认为非阻断。真机行为尚未验收，不得标记 ACCEPTED。

## 1. 冻结契约与责任边界

| 项 | 当前契约 |
|---|---|
| 插件 | `attribute_quality`，plugin version `0.1.0`。 |
| 能力 | `attribute.quality` v2.0；API 为带 `struct_size` 的 `query_item`、`query_option_context`、`evaluate` 三槽，三槽及大小均校验。 |
| 依赖/宿主 | required `item.facts` v3.3；当前 item facts 使用 Item Port v3.3；Game Host 当前 ABI 为 2.3，provider 校验 major=2 且 minor>=3，并要求四个 `read.attr_range` 函数。插件 descriptor 本身声明 capability requirement，不直接声明 Item Port requirement。 |
| 资源/设置 | 唯一资源 `hook:MATH_GetRandom`：private、required、fail-closed；feature `attribute_quality.provider` 是默认 1 的 Hidden/toggle，callback no-op，active 门禁由插件生命周期回调驱动。 |
| 不提供 | 无 HTTP/JNI；不写 item，不聚合整件装备，不负责 UI 颜色/文本、craft 策略、autosell 策略或材料清单。 |

契约与实现定位见 [`attribute_quality` 插件参考](../../../../reference/plugins/attribute_quality.md)，公开 feature 诊断字段见 [`api-reference.md` §7.7](../../../../reference/api-reference.md)。

## 2. 数值与输入设计

`AttributeQuality` 返回整数 `percentile` 与 `level`，level 取 `0..5`，最高级常量为 5。`evaluate` 使用 `int64_t` 计算 `(value - min) * 100 / (max - min)`，C++ 有符号除法向零截断。`max <= min` 固定返回 `{100,5}`。默认 `options=nullptr` 等价于 clamp；`allow_outside_range` 只接受 0/1，所有 reserved 字节必须为零。clamp 模式将低于 min 夹为 0、达到或超过 max 夹为 100；允许范围外值时保留未夹百分位，若结果超出 `int32_t` 返回 InvalidArgument。level 阈值依次为 30/60/75/90/100，对应 `1/2/3/4/5`，其余为 0。

输入与计算错误是不同层级：非法 options、空 output、范围转换溢出返回 `InvalidArgument`；不在区间内但允许范围外并且百分位可表示时是有效结果，不是错误。provider 不改变传入 value/range。

## 3. 读面、完整性与失败语义

### 3.1 `query_item`

一次调用读取 category、jewel/equip 分类，再取得一次 `QolItemInventoryView`；属性条目仅从该 view 按原序枚举，不按 ID 去重。`item.facts` view 的 `kQolItemInventoryOptionCapacity` 是 32；bit7 表示 completeness 元数据有效，bit8 只在容量已满且链表仍有下一项时表示 truncated。exact-32 且没有 next 不截断；33 项只返回前 32 项并令 `complete=0`。

`complete` 仅描述枚举完整度，不代表 range/quality 计算成功。输出条目保留 `ordinal`、attribute id、option type、value、range、quality 与独立 `status`；`ordinal` 只属于此次 view 顺序，不是跨查询身份或 UI 行号。失败项不从数组中跳过：stored option type 0 才探测，其他 type 在对应 ordinal 处写 `Unsupported` 并继续；Game Host 或 fixed-table 错误写入该项 status 后继续剩余项。ItemFacts 上下文或 view 结构无法读取时是外层错误。独立宝石结果恰为一项 `Jewel`；不属于宝石或装备的物品为 Unsupported。

### 3.2 `query_option_context`

参数是 `item handle + option_id + 当前 display_value`。provider 以该 ID 调同步 option range 查询，直接计算该 display value；不在储存 options 中搜索匹配项。结果 kind 为 `OptionContext`、ordinal 为 `UINT32_MAX`、option_type 为 `-1`。因此重复 option ID 的当前展示值可以各自计算，`display_value` 不表示已持久写回值。

### 3.3 固定表和错误保留

`uses_fixed_option_table` 成功且返回 0 时，范围使用捕获的普通范围；成功且返回 1 时必须取得 fixed percent pair，再按 `captured_max + captured_max * p/100` 转换，base<=0 按 1。固定表查找重复项最后一条生效。uses 查询错误、异常 flag、pair NotFound、NotReady 或范围转换越界均保留原错误，不能回退到捕获范围。批量 query 将故障保留在对应 item status；context query 则返回该错误并设置输出 status。

### 3.4 Capture 与运行门禁

范围读取同步调用 Game Host。捕获 scope 是 thread-local RAII：同线程嵌套返回 Busy；armed 后首次 `MATH_GetRandom(min,max)` 保存边界并返回 max，避免这次捕获消耗 RNG；scope 内其余 RNG 调用透传 backup；没有捕获到调用返回 NotReady。stored option 与 UI context 当前均以 flag 1 调 option getter，避免 getter 因非正 flag 不触发随机函数而留下未捕获范围（R01）。

provider query 需要 active、Game Host、item facts 与非空 backup；`evaluate` 不依赖这些 runtime pointers，也不检查 active。resource ready 对匹配 hook id 的空 backup 返回失败并关门禁，required/fail-closed claim 使资源安装失败时 provider 不启用。若 wrapper 在 backup 仍为空时被直接调用，代码返回 `INT_MIN` sentinel；该值不是游戏定义的错误值，**不能声称此调用在游戏侧安全**。正常契约依赖 resource-ready 拒绝、active 门禁与 query readiness 检查。

provider 的 `g_active` 是 atomic；Game Host、ItemFacts、backup 是共享借用指针。query/capture 在调用线程同步执行，不跨线程派发；Game Host adapter 对错误线程返回 Blocked。源码不承诺后台 `capabilities.require`/lease 与并发撤销安全。调用方应在本次同步调用内借用 capability pointer，不缓存跨撤销生命周期。

## 4. 消费者验收面

| 消费者 | 依赖关系 | 验收观察 |
|---|---|---|
| `autosell` | required `attribute.quality` | 只使用完整、count=1、Jewel、status=0 的 query_item 结果；质量不可用保持 `jewel_percentile=-1`，不以 0 误命中 gemRange。 |
| `attribute_range_display` | optional `attribute.quality` | 对当前 item/option/display value 调 query_option_context；成功时由 display consumer 映射颜色，失败/不可用不插色。 |
| `custom_recipe` | optional `attribute.quality` | query 当前宝石；通过 `evaluate` 纯计算候选档位。质量查询失败在候选随机、item write 和材料需求更新之前退出；成功写回与材料策略仍由 recipe owner 负责。 |

三者均不把 provider API pointer 缓存跨 revoke。provider 只提供属性事实与纯计算，消费者拥有各自的呈现/规则/写路径。

## 5. 原始 Oracle Findings AQ01–AQ08 与整改映射

下表保留 Oracle 原始 AQ01–AQ08 编号及 finding 含义，逐项映射代码整改和 Host 证据；不重新定义编号，也不把 Host 结果当作 VM 结论。AQ 条目均不标 ACCEPTED/CLOSED。

| ID | 原始 finding 与整改映射 | Host 证据 | 状态 |
|---|---|---|---|
| AQ01 | signed overflow 风险：range width、numerator multiply、结果/边界加法须宽算术并检查窄化溢出；实现于 `rules.cpp` 的 int64 运算及范围溢出错误。 | `attribute_quality_tests` 114 assertions，覆盖极值/overflow。 | 整改已实现；不销 AQ。 |
| AQ02 | fixed option category/uses/pair 错误不应被忽略或 fallback；保留 NotReady/NotFound，batch 写逐项 status；另由 R02 增加 GOT 中间引用空值保护。 | `attribute_quality_provider_tests` fixed-table cases；`attribute_quality_adapter_tests` non-applicable/Found/NotFound/NotReady 与 R02 null-GOT cases。 | 整改已实现；不销 AQ。 |
| AQ03 | Host `QolResult` 不得折为 bool 丢失 Blocked/stale 等错误语义；实现为逐层保留原 `QolResult`，每条 batch item 保留 status。 | provider wrong-thread、stale handle、opaque Host error cases；最终集成 CTest。 | 整改已实现；不销 AQ。 |
| AQ04 | 嵌套 capture 会覆盖 TLS 状态；拆分 `capture_active` / `capture_armed` 并以 RAII 清理，嵌套调用返回 Busy。 | `test_capture_scope_nested_guard` 覆盖捕获前/后的嵌套；最终 provider target。 | 整改已实现；不销 AQ。 |
| AQ05 | recipe 候选 quality 错误不能降级为 Grey 后继续；改为 `QolResult` 错误传播，查询失败不调用候选 random、不写 jewel、不更新材料数。 | `custom_recipe_tests`、`custom_recipe_handle_chain_tests` 2/2；最终集成 CTest。 | 整改已实现；不销 AQ。 |
| AQ06 | 既有 rules/fake lifecycle/disabled tests 不足以证明真实 ItemFacts view 和 adapter 完整性；新增真实 provider/Item Port/Game Host adapter/消费者/recipe 受影响 targets。 | final integration CTest 14/14；覆盖 0/31/32/33、exact-32/33 truncation 和真实 adapter 路径。 | 整改已实现；VM 仍 NOT_ACCEPTED。 |
| AQ07 | 空 backup 曾可被 ready 接受、无 backup wrapper 会返回 `min`；现 resource-ready 对空 backup 拒绝并关 active，query 要求 live backup。当前 wrapper 的 `INT_MIN` sentinel 不是游戏可识别错误，不能声明该意外调用安全。 | R01 focused tests；`test_backup_fail_closed_and_disabled_evaluation`；最终集成 CTest。 | 门禁整改已实现；sentinel VM 风险未验证，不销 AQ。 |
| AQ08 | 公共 contract 注释错误指向 ARCH §7.4；注释已改为与当前只读职责/呈现边界一致。 | 源码注释核对；相关三槽 contract 编译与 provider descriptor tests。 | 注释整改已实现；非行为 Host/VM 结论，不销 AQ。 |

## 6. VM-AQ-01（独立验收卡，当前 NOT_ACCEPTED）

**范围**：Debug monster 集成包中的 `attribute_quality` v2.0、required `item.facts` 3.3、resource `hook:MATH_GetRandom`、required/optional consumers、正常查询与撤销行为。质量 API 本身没有 HTTP/JNI route；API 只用于查 descriptor 与 resource 诊断，不以 HTTP 暴露质量结果。

**部署候选**：`output/inotia4_qol_lsposed_debug_2610141000_10092b3392bd.apk`；SHA-256 `10092b3392bdbc6319144f0095698a32990e72da7b386d1bc9820bf93be4ad4d`。这是构建产物身份，不是设备运行模块身份。设备 serial、模块 identity SHA 与嵌入 APK SHA 的现场核对尚无记录。

**步骤与观测**：按 `docs/guides/build-and-deploy.md` 当前集成部署流程安装并确认模块身份；启动后确认 `/api/system/features` 中 descriptor 的 `attribute.quality` v2.0 / `item.facts` 3.3 声明及 provider 状态，`/api/system/resources` 中唯一 hook 为 live；在游戏内取装备储存词缀和宝石，验证 detail display 当前值着色、autosell gemRange 阈值及 recipe tier-up 读取同一质量规则；再对 provider 执行 disable/deactivate，确认 required autosell 的依赖状态和 optional consumer 的 fail-closed/degraded 行为，恢复后按生命周期语义复核。记录版本变体、设备 serial、module/APK SHA、logcat 与屏幕/存档前后状态。

**失败判据**：错误/不完整 view 被当成完整质量结果；fixed-table NoMatch/NotReady 静默回退；同线程嵌套 capture corrupts 外层范围；backup 缺失仍开放查询；provider revoke 后消费者继续用缓存接口；失败 recipe 查询仍调用随机/写宝石/改材料；展示值或自动出售用默认 0 误命中。

**当前结论：NOT_ACCEPTED**。本范围静态整改、Host CTest 与 Debug build 已完成，Oracle 对 R01/R02 的限定复核无新增阻断；这些证据不替代设备行为。VM-K2-02 历史行保持原样；其早期 provider/consumer 验证不等同于当前 v2.0 contract 的本卡验收。
