# `stack_limit` 插件参考

## 摘要

功能插件（声明式资源 owner、叶子）：拥有「999 数量扩展」的 16 条调用点 patch 原子组和唯一开关 `stack_limit.limit_999`；两条存档 bitend claim 归 Host 常驻安装，不属于插件资源组。插件不提供能力、无 HTTP 路由、无面板、无命令、无 JNI 行为，也不在插件内执行数量编解码；S2 布局与 99/999 视图归 Host 常驻 Adapter（`game/adapter_ports/item_count_codec.h`、`game/adapter_ports/item_count_facade.cpp`）。

除特别说明，文中路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`stack_limit`，插件版本 `0.1.0`（`plugins/stack_limit/plugin.cpp:15-16`）。descriptor：`requires_count=0`、`optional_requires_count=0`、`provides_count=0`、`jni_count=0`、无 HTTP query/endpoint/route metadata（`plugin.cpp:89-104`），无 `routes.inc`。注册于 `host/assembly.cpp:231`，头文件在 `host/assembly.cpp:64-65` 引入。

插件声明 16 条资源；两条存档 bitend claim 由 `host/core_patches.cpp` 作为 Host 常驻 patch 注册并尝试安装，不随插件资源组启停。

文件规模：`plugin.cpp` 113 行、`plugin.h` 12 行、`settings_contribution.*` 15 行、`CMakeLists.txt` 4 行。

## 公开能力与路由

无对外能力：不 `provides`、不注册 HTTP 路由、快照、事件、命令或 JNI 导出。

唯一用户可见 feature（`plugin.cpp:83-87`）：

| 字段 | 值 |
|---|---|
| id | `stack_limit.limit_999` |
| default | `0`（关闭） |
| category / type | `kGameplay` / `kToggle` |
| name | 堆叠上限 |
| description | 将可堆叠物品上限提高到 999。 |
| `on_toggle` | no-op（返回 `ok()`） |

运行条件由 Host 的 `enable && any(feature)` 派生，资源组安装/还原由 Host 生命周期执行；toggle 回调本身不做任何动作。

配置面：设置页按 descriptor features 渲染，`settings_contribution.cpp:6` 的 `register_settings` 是已退休的空实现（返回 `ok()`）。旧扁平键 `stackLimitIncrease` 仍作为 owner-token 设置桥的公开布尔键映射到 namespace `stack_limit.enable`（`host/settings_config_bridge.cpp:28`），写入经同一 Host Store；legacy `ConfigStore::update` 在 feature model 下拒绝（`host/core_settings_config_store.cpp:1530`），迁移映射见 `core_settings_config_store.cpp:444-458,480-495`。单独将该键置 true 不满足 `want_run = desired_enable && any_feature`。`stackLimitIncrease` 与 feature `stack_limit.limit_999` 是两个不同布尔：前者不驱动后者。

`NativeBridge.nativeSetStackLimitEnabled` 是冻结兼容 JNI，忽略参数固定返回 `JNI_FALSE`（`bridge/native/gamebridge_domain_operations.cpp:443-449`），Kotlin 侧仅保留声明（`module/app/src/main/java/com/inotia4/qol/NativeBridge.kt:218`），无调用点。该 JNI 不属于插件 descriptor。

## 游戏内行为

16 条资源 claim（`plugin.cpp:20-45`），统一字段：`ResourceMechanism::kCallPatch`、`ResourcePolicy::kPrivate`、`ResourceRequirement::kRequired`、`ResourceFailure::kFailClosed`、`resumable=true`、`owner="stack_limit"`、`gate=nullptr`、`step_budget=120`、`handler=nullptr`、`atomic=true`。失败原因文案说明 999 数量组的 S2 布局与数量上限需整组安装（`plugin.cpp:17-18`）；未应用时的读取与写入语义见下文。

站点实体在 `game/access/patches.cpp:60-100`，目标符号 VMA 来自 `game/symbols/game_symbols.h`，插件不持有裸地址。

| claim | 站点（文件:行） | VMA + 偏移 | 改写 |
|---|---|---|---|
| `P_INVEN_MOVE_ITEM_CLAMP_150` | `patches.cpp:67-68` | `0x104934 + 0x150` | `0x71018c1f → 0x710f9c1f`（cmp #99→#999） |
| `P_INVEN_MOVE_ITEM_CLAMP_158` | `patches.cpp:69-70` | `0x104934 + 0x158` | `0x52800c77 → 0x52807cf7`（mov #99→#999） |
| `P_INVEN_SAVE_ITEM_DIRECT_CLAMP_DC` | `patches.cpp:71-72` | `0x103bf0 + 0xdc` | `0x71018c7f → 0x710f9c7f` |
| `P_INVEN_SAVE_ITEM_DIRECT_CLAMP_F0` | `patches.cpp:73-74` | `0x103bf0 + 0xf0` | `0x71018e9f → 0x710f9e9f` |
| `P_INVEN_SAVE_ITEM_DATA_CLAMP_7C` | `patches.cpp:75-76` | `0x104614 + 0x7c` | `0x71018a9f → 0x710f9a9f` |
| `P_INVEN_FIND_SAVE_SLOT_CLAMP_238` | `patches.cpp:77-78` | `0x103960 + 0x238` | `0x71018c1f → 0x710f9c1f` |
| `P_UISTORE_BUTTON_SELL_EXE_D18D0_ARG` | `patches.cpp:81-82` | `0xd1818 + 0xb8` | `0xb9401260 → 0xaa1303e0`（改传物品指针） |
| `P_UISTORE_BUTTON_SELL_EXE_D18D0_CALL` | `patches.cpp:83-84` | `0xd1818 + 0xc4` | `0x9401bb13 → BL F_GET_CUMULATE_COUNT_VMA` |
| `P_UISTORE_BUTTON_SELL_EXE_D18E8_ARG` | `patches.cpp:85-86` | `0xd1818 + 0xd0` | `0xb9401260 → 0xaa1303e0` |
| `P_UISTORE_BUTTON_SELL_EXE_D18E8_CALL` | `patches.cpp:87-88` | `0xd1818 + 0xdc` | `0x9401bb0d → BL F_GET_CUMULATE_COUNT_VMA` |
| `P_UISTORE_SELL_ITEM_D26B8_ARG` | `patches.cpp:89-90` | `0xd25f0 + 0xc8` | `0xb9401260 → 0xaa1303e0` |
| `P_UISTORE_SELL_ITEM_D26B8_CALL` | `patches.cpp:91-92` | `0xd25f0 + 0xd4` | `0x9401b799 → BL F_GET_CUMULATE_COUNT_VMA` |
| `P_ITEMSYSTEM_DIVIDE_108458_ARG` | `patches.cpp:93-94` | `0x1083f8 + 0x60` | `0xb9401260 → 0xaa1303e0` |
| `P_ITEMSYSTEM_DIVIDE_108458_CALL` | `patches.cpp:95-96` | `0x1083f8 + 0x6c` | `0x9400e031 → BL F_GET_CUMULATE_COUNT_VMA` |
| `P_ITEMSYSTEM_DIVIDE_10848C_ARG` | `patches.cpp:97-98` | `0x1083f8 + 0x94` | `0x2a1603e0 → 0xaa1303e0` |
| `P_ITEMSYSTEM_DIVIDE_10848C_CALL` | `patches.cpp:99-100` | `0x1083f8 + 0x98` | `0x9400e026 → BL F_GET_CUMULATE_COUNT_VMA` |

16 条资源由 6 条 clamp 与 10 条直接位读重定向构成：clamp 把「数量完整值」判定上限从 99 提到 999；重定向把直接位读的实参改为物品指针、调用改指 `ITEM_GetCumulateCount`，使读侧统一解码完整数量。

两条存档 bitend claim 由 Host 常驻设施管理，不属于上述 16 条插件资源：`host/core_patches.cpp:43-73` 定义 `kSaveInventoryBitendClaim` 与 `kLoadInventoryBitendClaim`，字段为 `kCallPatch`、`kPrivate`、`kOptional`、`kFailSoft`、`resumable=true`、`owner="host"`、`reason="999 数量组存档位段守卫（常驻）"`、`gate=nullptr`、`step_budget=120`、`atomic=false`。`install_bitend()`（`core_patches.cpp:97-112`）安装失败只记录 `QOL_LOG_WARN`；`core_patches_install()`（`:163-188`）注册并尝试安装两条 claim，失败不阻断 boot。

两处分别位于 `SAVE_SaveInventory+0x78` 与 `SAVE_LoadInventory+0xa8`，将 `UTIL_GetBitValue(bag_item+0x10, 24, 0)` 改为 `(21, 0)`。该调用返回袋容量，决定袋内物品 save/load 循环次数（`game/bag.md:46-58`）；袋容量实际值为 4/8/12/16（`:62-74`），bits22–24 恒为 0。收窄读取位段是为了避免 S2 的 `a`（bits22–24）被当作袋容量。

S2 编码布局（`game/adapter_ports/item_count_codec.h`）：a 段 bits22–24、b 段 bits25–31，数量 = 128a+b，编码容量 1023（`item_count_codec.h:19-28`）。999 未应用时，`effective`/`view_from_canonical` 读取 b 段（0–127，不夹 99）；`move`/`save`/`divide`/`consume`/`remove`/`borrow` 的模块 S2 写回跳过，由原版只写 b、保留 a。`write_effective`、`field_from_canonical`、`view_limit` 的关闭态写侧上限仍为 99；999 已应用时上限为 999（`item_count_codec.h:12-17`）。

## 使用的 Host SDK 与 Game 面

- 插件 `install` 只校验 `ctx.struct_size`、`ctx.sdk`、`ctx.game` 非空，不保存 `ctx`、不调用任何 SDK/Game 函数（`plugin.cpp:53-59`）。
- 不 `require` 能力，不注册消费者，不使用快照、事件、命令端口或面板登记。
- 唯一使用的机制是资源声明；站点目标、原字校验、物理写入与还原由 `patch/` 与 Host 生命周期执行。

## 消费者与职责边界

插件不 `provides`，因此没有插件级依赖边、`blocked` 或级联停用。数量 API 有两类消费者：① 插件经 Host 注入的 `QolGameHost.item_count`（`contracts/game_host.h:60-76`，12 项函数）访问，其行为受本插件资源组有效态驱动；② Adapter 内部路径直接取 `qol::game::item_count_api_if_resident()`，例如 `game/adapter_ports/item_port.cpp:117-138`、`game/read/item_class.cpp:17`、`core/native/inventory_trade.cpp:54,144`：

| 消费者 | 入口 |
|---|---|
| `core.item` | Item Port `read.effective_cumulate_count` / `count_limit` / `count_encoding`（`plugins/core.item/plugin.cpp:155-172,334`） |
| `inventory_hooks` | `plugins/inventory_hooks/native_inventory_hook.cpp:247-315,372-382,445,464,1305` |
| `extension_bag` | `plugins/extension_bag/game_ui_virtbag.cpp:680,752-861,1391` |
| `core/native/inventory_trade.cpp` | `:54,144`（交易与日志数量） |
| `game/adapter_ports/item_port.cpp` | `:110-145`（`item_count_active()` 时读 canonical，否则原版 getter） |
| `game/read/item_class.cpp` | `:17` |
| `bridge/native/gamebridge_domain_operations.cpp` | `:437`（调试 export） |
| `host/item_inventory_state_write_port.cpp` | `:145-150`（`add_item` 优先使用显式 provider，否则取 resident API 并转交 `ops::add_item`）|
| `game/access/inventory_legacy_read.cpp` | `:109-127`（直接取 resident API，通过 `effective` 读取数量视图并写入旧库存输出）|
| `game/ops/item_inventory_state_write.cpp` | `:323,371-400`（`add_item` 用 `write_effective` 写入数量；必要时回退 `field_from_canonical(..., 0, ...)`）|

`ItemCountExtendedMode`（`game/adapter_ports/item_count_facade.h:7-13`）：`kOff`=请求明确关闭；`kNotReady`=请求开启但资源组/适配未满足；`kApplied`=已应用。`read_extended_mode` 在 `kNotReady` 返回 `kQolErrNotReady`，不伪装关闭（`item_count_facade.cpp:150-162`）。

`effective`、`write_effective`、`writeback_needed` 每次调用各读取一次 `snapshot_mode()`（`item_count_facade.cpp:59-64,82-87,97-102`）；三态和编码类别的行为语义不变。

边界归其他 owner：S2 编解码与视图转换归 Game Adapter（`item_count_codec.h:5-10` 明示插件不得 include）；资源组仲裁、安装、回滚与原子组判定归 Host（`host/plugin_registry.cpp:1060-1226`）；patch 目标与写入归 `game/access/patches.cpp` 与 `patch/`；配置 schema/Store 归 Host 常驻管理设施；设置页渲染归 `settings` 插件。架构口径见 `ARCHITECTURE.md:336`。

## 失败语义

- 16 条插件 claim 全部 `kRequired + kFailClosed + atomic`：任一安装失败触发 `rollback_atomic_group` 逆序还原组内已 live/disabled 成员（`host/plugin_registry.cpp:1094-1143,1209-1226`），插件进入 `failed`。
- 两条 Host 常驻 bitend claim 为 `kOptional + kFailSoft`、`atomic=false`；安装失败只记录 `QOL_LOG_WARN`，不阻断 boot，也不属于 `stack_limit` 原子组（`host/core_patches.cpp:43-73,97-112,182-188`）。
- 请求开启但资源组未落地时，`sync_item_count_policy()` 置 `kNotReady`（`plugin_registry.cpp:1516-1535`）；不伪装为关闭，消费者读 `read_extended_mode` 得到 `NotReady`。
- 插件资源组未安装时，6 条 clamp 与 10 条重定向缺席；关闭态读取返回 b 段（0–127，不夹 99），写操作按原版只写 b、保留 a，模块 S2 写回不执行。`write_effective`、`field_from_canonical` 与 `view_limit` 的关闭态写侧上限仍为 99；Host 常驻 bitend 不随该组启停。
- `kApplied` 判据：无在途生命周期事务、status 为 `kEnabled`、`enabled` 为真、组内全部 atomic claim live（`plugin_registry.cpp:1516-1535`）。撤销事务一建立即退出 applied。

## 生命周期

- `install` 校验 context；`deactivate`/`on_disabled` 置 `g_enabled=false`；`on_enabled` 置 true（`plugin.cpp:53-74`）。
- `fill_status` 只上报 `g_enabled`，注释说明该变量仅用于诊断（`plugin.cpp:50-51,76-79`）；资源有效态与数量策略不读取该变量。
- 默认 feature 关闭，因此启动后插件不运行、资源保持 `free`/`disabled`；开启 feature 后由 Host 帧任务安装原子组，再经 `sync_item_count_policy()` 切到 `kApplied`。
- `settings_contribution` 已退休且 assembly 未调用；测试仍将符号纳入 `tests/test_settings_contributions.cpp:39`。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/stack_limit/plugin.cpp` | `:15-18` id/version/reason；`:20-45` 16 条 claim；`:51-72` lifecycle；`:74-77` 状态上报；`:81-103` feature 与 descriptor |
| `plugins/stack_limit/plugin.h` | 入口声明 |
| `plugins/stack_limit/settings_contribution.cpp` | `:6` 退休空实现 |
| `game/access/patches.cpp` | `:60-100` 16 条插件资源与 2 条 Host 常驻 bitend 的站点定义 |
| `game/adapter_ports/item_count_codec.h` | `:12-17` 上限；`:19-28` S2 布局；`:30-93` 编解码与视图转换 |
| `game/adapter_ports/item_count_facade.cpp` | `:59-115` facade 模式读取与转换；`:150-162` `read_extended_mode` |
| `game/adapter_ports/item_count_facade.h` | `:7-13` `ItemCountExtendedMode` |
| `contracts/game_host.h` | `:60-76` `QolItemCountApi` |
| `host/plugin_registry.cpp` | `:1060-1226` 原子组安装/回滚；`:1516-1535` 数量策略同步 |
| `host/core_patches.cpp` | `:25` bitend reason；`:43-73` 两条 Host 常驻 claim；`:97-112` `install_bitend()`；`:163-188` claim 注册与安装 |
| `host/core_settings_config_store.cpp` | `:444-458,480-495` legacy 键映射；`:1530` feature model 拒绝 legacy update |
| `tests/test_stack_limit_plugin.cpp` | `:53` 断言 16 条资源；`:60-77` 资源 ID；`:78-94` 5 组 ARG/CALL 配对；`:95-108` claim 字段与无 bitend |
| `tests/test_core_patches_shape.cpp` | `:19-29` 检查 core_patches 含两条 bitend claim、且不含 S2 重定向 claim |
| `tests/test_item_count_codec.cpp` | `:91-129` S2 编解码边界矩阵 |
| `tests/test_item_count_facade.cpp` | `:96-117` 三态；`:158-217` 编码/非编码类别与状态转换 |
