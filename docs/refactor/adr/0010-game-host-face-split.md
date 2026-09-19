# ADR-0010：P2 Game Host face 拆分冻结

## 状态

已冻结目标面；本批仅实施 P2-A 的日志与存档事件迁移。Game Host 的具体拆表留给
P2-B，不在本 ADR 中提前实现。

## 背景与冻结结论

ora-8 已核验当前 P2 face 存在以下边界问题：`QolGameHostExtraApi` 是跨域万能表，
一次聚合 character、item、inventory、craft、world、save、quest、choice；不能直接
冻结为插件公共面，否则违反接口隔离。后续 P2-B 必须拆成：**最小只读读面、provider
capability、owner command**。

本 ADR 冻结以下拆分原则：

- 日志只能经 Host SDK `log.write(level, domain, source, message, len)`，插件需要宏时
  只能使用自己的薄封装头。
- 进档/退档是 Host SDK 事件，不再由插件注册宿主私有回调；旧
  `save_enter_register/unregister`、`save_exit_register/unregister` 在所有消费者迁移
  前继续保留。
- `QolGameHostExtraApi` 不作为跨域总表冻结；只读事实由最小 Game Host 读面或 provider
  capability 提供，副作用由 owner command 提供。

## ora-8 符号面清单

下表记录 19 个 legacy 文件的当前符号面与目标面。目标面是迁移方向，不表示本批已全部
完成。

| 文件 | legacy 头/面 | 目标面 |
|---|---|---|
| `plugins/extension_bag/game_ui_virtbag.cpp` | `game_*`、`game_symbols.h`、`core/native/*` | Host SDK UI/事件/调度 + extension_bag/inventory owner capability |
| `plugins/inventory_hooks/native_inventory_hook.cpp` | `qol_log.h`、`extension_bag_port.h`、`item_class.h`、游戏头 | Host SDK log + item read facade + inventory owner command |
| `plugins/core.ui/panels/settings.cpp` | `core/native/*`、根部 `game_*`、`qol_log.h` | Host SDK UI/log + core.ui 自有面板契约 |
| `plugins/autosell/scan.cpp` | `inventory_trade.h`、`qol_log.h`、`save_enter/exit.h` | Host SDK log/events + inventory sell owner/capability |
| `plugins/save_backup/ui/panel.cpp` | `game_ops_common.h`、`qol_log.h` | Host SDK UI/log + save_backup 自有面板 |
| `plugins/save_backup/feature.cpp` | `game_feature.h`、`game_ops_common.h`、`qol_log.h` | Host SDK log + save read facade + save owner command |
| `plugins/extension_bag/extension_bag_port.cpp` | `core/native` 端口、`sell_price.h` | extension_bag capability 与 inventory owner 面 |
| `plugins/extension_bag/extension_bag_geometry.cpp` | `game_*`、UI 私有头 | Host SDK UI adapter |
| `plugins/autosell/ui/panel.cpp` | `item_class.h`、`game_access.h`、`qol_log.h` | Host SDK UI/item read/log |
| `plugins/inventory_hooks/inventory_hook_stage4.cpp` | `sell_price.h`、`game_symbols.h` | inventory owner capability/command |
| `plugins/autosell/view.cpp` | `item_class.h`、`game_access.h` | Host SDK item read/provider capability |
| `plugins/extension_bag/game_ui_virtbag.h` | `game_*`、`game_symbols.h`、端口头 | contracts + extension_bag 私有头 |
| `plugins/extension_bag/extension_bag_store.inc` | `game_*`、store legacy 面 | Host SDK store + extension_bag owner |
| `plugins/extension_bag/extension_bag_persistence.cpp` | `qol_log.h`、sidecar legacy 面 | Host SDK log/store；module save participant 延后 |
| `plugins/extension_bag/extension_bag_logging.h` | `qol_log.h` 宏 | extension_bag 私有 Host SDK log 薄封装 |
| `plugins/autosell/view.h` | `item_class.h` | contracts item read/provider capability |
| `plugins/autosell/store.cpp` | `qol_log.h`、store legacy 面 | Host SDK log/store |
| `plugins/autosell/plugin.cpp` | `qol_log.h`、插件装配面 | Host SDK log/events/panels |
| `plugins/save_backup/plugin.cpp` | `qol_log.h`、插件装配面 | Host SDK log/panels |

表中每个文件只计一次；完整文件口径为 19 个，来自 ora-8 对当前源码的逐文件清单。

## 缺面与签名建议

### 1. item class 只读缺口

当前只读面仍缺少：`enchant_remaining`、`socket_filled`、`socket_total`、
`is_backpack_category`。建议由 item provider capability 提供固定宽度 POD 函数表：

```c
QolResult (*enchant_remaining)(const void* item, int32_t* out_value);
QolResult (*socket_filled)(const void* item, int32_t* out_value);
QolResult (*socket_total)(const void* item, int32_t* out_value);
QolResult (*is_backpack_category)(int32_t category, int32_t* out_value);
```

所有函数只读、主线程调用；未解析、空对象和越界统一返回 `QolResult` 错误，不以默认值
掩盖未就绪状态。

### 2. save 事件

事件契约固定为：`kEventSaveEnter` / `kEventSaveExit`，载荷分别为
`SaveEnterEvent` / `SaveExitEvent`，唯一字段为 `int32_t slot`。发布点保持 legacy
触发点：进档检测任务在确认 world 就绪后发布，退档 wrapper 在调用原
`GAME_Exit` 前发布；均在游戏主线程，进档/退档先后顺序不变。

### 3. 库存出售 owner/capability

只读 item/库存事实走最小 provider capability；出售、数量扣减、金币结算和跨原版/扩展
背包编排归 inventory/extension_bag owner。建议命令入口使用
`QolCommands::post(QolCommandTicket*, const QolCommand*)`，命令 payload 只含槽位、物品
身份和数量，执行在主线程；插件不得以跨域 `extra` 直接调用出售副作用。

### 4. 明确延后项

- module save participant 延后，当前 sidecar 与扩展背包持久化不在本 ADR 中合并。
- world、quest、craft 的副作用迁移为各自 owner command；Game Host 只保留必要低层读面。
- `QolUiPrimitiveApi` 与 `QolUiAdapter` 不得并存重复；统一以 `contracts/ui.h` 的
  标准组件契约和唯一 adapter 为准。

## 必须拆除的五项设计

1. 拆除 `QolGameHostExtraApi` 的跨域聚合，改为最小只读读面/provider capability/owner
   command。
2. 删除或限定宿主内部的 `QolCallApi::call_known` 泛型逃生口，插件不得以它绕过稳定契约。
3. 消除 `QolUiPrimitiveApi` 与 `QolUiAdapter` 的重复面，只保留唯一 UI adapter 方向。
4. 禁止插件 include `game_symbols.h`；游戏版本常量继续只由 `game/` 私有实现持有。
5. 删除 `contracts/autosell.h` 的全局函数模式，改为明确的 provider/Host SDK port。

## Lane 顺序与验收点

| Lane | 内容 | 验收点 |
|---|---|---|
| P2-A | Host SDK `log` 使用者迁移；进/退档发布 `SaveEnter/SaveExit` 事件；保留 legacy 注册 API | 插件不再 include `qol_log.h`、`save_enter.h`、`save_exit.h`（受禁止改动文件例外须单独登记）；host events 测试含 slot 载荷、订阅/注销与时序说明 |
| P2-B | 拆 `QolGameHostExtraApi` | fake host 测试覆盖最小只读面、capability lookup、owner command、错误/未就绪语义 |
| P2-C | item class 缺面、库存出售 owner/capability、save owner 面 | autosell/inventory/save 的受影响 VM 卡覆盖副作用主线程、数量与失败回滚 |
| P2-D | UI 重复面、symbols include、autosell 全局函数清除 | UI adapter 单一入口、插件独立 include 审查、contracts 无全局业务函数 |
| P3 | 19 legacy include 清零、独立 target/链接矩阵、校验器删除过渡允许项 | `check_arch.py` 对插件越层和符号头硬失败；每个插件可独立编译，受影响行为有真机卡 |

每个 lane 只移动职责，不顺手重写业务算法。行为面验收必须使用受影响 VM 卡的真机日志；
host 测试、Debug APK 或 VM-BOUNDARY 不得替代该证据。

## 风险与限制

- VM-BOUNDARY 只证明启动、门禁和生命周期 smoke，不证明插件边界或独立链接边界。
- 单一 target 即使构建成功，也不证明插件可独立编译或不链接宿主私有符号。
- 本批按任务要求不执行真机安装、force-stop、API 轮询和行为回归；因此行为面只能记录为
  `NOT_ACCEPTED`，不能用 host 测试或 Debug 构建替代真机证据。
- legacy 回调 API 在全部消费者迁移前必须保留；事件总线使用快照派发，注销当轮可能仍被
  调用一次，消费者不得依赖“注销后当轮立即生效”。

## P2-B1：按域只读面与 item 缺口（纯加法）

本批在 `QolGameHost` 尾部追加 `read_only`，不改变既有成员的顺序、偏移或签名；
`kGameHostMinor` 从 2 递增为 3。`read_only` 下列子表只包含读事实，函数统一返回
`QolResult`：

| 域 | 新面成员 | 实现落点 |
|---|---|---|
| character | `type`、`is_active_player_group`、`party_index`、`is_party_member`、`summoner`、`level`、`merc_slot`、`can_change_equip`、`menu_character`、`pool_slot`、`max_hp`、`hp` | `host/game_host.cpp` → `game/access/char.*` |
| item | `category`、`is_jewel_category`、`ability_level`、`rarity`、`is_real_equip`、`equip_level`、`is_no_sell`、`is_dice`、`cumulate_count`、`class_no_equip_bit`、`equip_desc_role_type`、`enchant_remaining`、`socket_filled`、`socket_total`、`is_backpack_category` | `host/game_host.cpp`；缺口原语 → `game/access/item_class.*` |
| world | `position`、`state`、`map_record_count`、`current_map_id`、`map_name`、`active_player_direction`、`money` | `host/game_host.cpp` → `game/access/world.*` |
| craft | `ui_type`、`panel_state`、`mix_type`、`selected_stuff_index`、`selected_stuff_item`、`stuff_item`、`category_held_count`、`character_level`、`stuff_slot_control`、`target_slot_item`、`selected_inventory_item`、`selected_recipe`、`stuff_slot_count` | `host/game_host.cpp` → `game/access/craft.*` |
| save | `slot_state`、`slot_exists`、`slot_hero_index`、`hero_level`、`hero_class`、`load_data`、`hub_key` | `host/game_host.cpp` → `game/access/save.*` |
| quest | `state`、`active_slot_count`、`active_slot_id`、`total_count`、`quest_state`、`is_complete` | `host/game_host.cpp` → `game/access/quest.*` |
| choice | `state_id`、`selected_index`、`text_owns` | `host/game_host.cpp` → `game/access/ui_choice.*` |

以上成员的完整 C 签名以 `contracts/game_host.h` 的 `Qol*ReadOnlyApi` 声明为准；指针、
数组和存档数据继续遵守既有所有权契约。item 缺口的位域知识只在
`game/access/item_class.*`：`I_ENCHANT` bits2–5、`I_SOCKET` 低/高 nibble，以及
`ITEMCLASSBASE` 记录 +2 的 `0x1f` 判定；插件不获得 `I_ENCHANT`/`I_SOCKET`/`I_COUNT`/`g_base`。

`QolGameHostExtraApi` 保留为 deprecated 兼容 shim，继续指向既有实现；P2-B2/B3 迁移
消费者后于 P2-B4 删除。`QolCallApi::call_known` 与 `QolUiPrimitiveApi` 均标为
P2-B4 待删，UI primitive 的替代面为已覆盖的 `UiAdapter`；本批不删除，避免消费者
编译断裂。

## 后续 lane 切分与验收

- **P2-B2**：迁移 character/item/world/craft 的只读消费者，改用 `read_only`，并补
  provider capability lookup；验收为旧 `extra` 读成员无新增消费者、错误码保持
  `invalid_argument/not_ready`。
- **P2-B3**：迁移 save/quest/choice 读消费者与 inventory/item provider；副作用改由
  owner command 承载，验收为主线程执行、失败不写默认值、相关消费者不再依赖 `extra`。
- **P2-B4**：完成所有消费者迁移后删除 `extra`、`call_known` 与 `QolUiPrimitiveApi`
  兼容面，统一 `UiAdapter`；验收为 ABI minor/size 断言、全量 host tests、插件独立
  include/link 检查和受影响 VM 卡真机日志。

本批 host adapter 测试使用 fake game access 覆盖正常、空指针和未就绪错误；fake 只
证明转发表和错误语义，不证明真实游戏内存位域的运行时布局。按 ADR 的行为证据规则，
本批不装真机，行为面仍为 `NOT_ACCEPTED`。

## 模块侧插件原型（simple_mode）

### CMake 边界做法

`module/app/src/main/cpp/CMakeLists.txt` 为 `plugins/simple_mode/*.cpp` 建立独立的
`qol_plugin_simple_mode` 静态 target，target 只编译 `rules.cpp`、`feature.cpp` 和
`plugin.cpp`，并将其以 `PRIVATE` 方式链接进 `gamebridge`。这三个源文件不再作为
`gamebridge` 的源文件重复编译，因此插件实现的链接入口已经由独立 target 提供，装配
顺序和运行时注册代码仍在宿主 `host/assembly.cpp`。

该 target 的 include 目录严格只有：

```cmake
${CMAKE_CURRENT_SOURCE_DIR}/contracts
${CMAKE_CURRENT_SOURCE_DIR}/plugins/simple_mode
```

没有继承 `gamebridge` 的 include 目录，也没有把 `${CMAKE_CURRENT_SOURCE_DIR}` 加入
插件 target。插件源文件使用自身目录的头和 contracts 目录的契约；contracts 内部头
也使用同目录引用，以便在不开放宿主根目录的情况下完成传递包含。故插件 target 对
`host/`、`infra/`、`game/`、`hooks/`、`patch/`、legacy 目录均无可用 include 搜索路径。

边界复现：在 `plugins/simple_mode/plugin.cpp` 临时加入
`#include "host/assembly.h"`，执行 Android native 构建或直接对该 target 构建时应因
找不到头文件失败；删除该行后构建恢复。该行不应保留在源码中。

### simple_mode 实际 ABI 清单

| ABI 面 | 实际使用 | 状态 |
|---|---|---|
| `PluginDescriptor` | `id`、`version`；`requires`/`optional_requires`/`provides` 的数量与空指针；`resources_count/resources`；`jni_count/jni`；`install`、`deactivate`、`on_enabled`、`on_disabled`、`fill_status`、`on_resource_ready` | 已够用 |
| `ResourceClaim` | 两条 `kEntryHook` 声明：`id`、`symbol`、`policy`、`requirement`、`failure`、`resumable`、`owner`、`reason`、`gate`、`step_budget`、`handler`；`atomic` 使用默认 `false` | 已够用 |
| `JniBinding` | `kotlin_method`、`native_symbol`、`signature_hash=nullptr` | 当前已够用；签名哈希值属于 P5 后续缺面，不阻塞本原型 |
| `PluginContext` | `struct_size`、`sdk_major`、`sdk`、`game`；`sdk_minor/plugin_id` 由宿主填充但本插件不读取 | 已够用 |
| Host SDK `log` | `QolHostSdk::log.write(level, domain, source, message, message_len)` | 已够用 |
| Host SDK `config` | `QolHostSdk::config.get_bool(key, default_value, out_value)` | 已够用 |
| Host SDK `events` | `subscribe(type, handler, ctx, out_id)`、`unsubscribe(id)` | 已够用 |
| 事件类型 | `kEventConfigChanged` 与 `ConfigChangedEvent{key, int_value}` | 已够用；不使用 save 事件 |
| Game Host 角色读面 | `read.character.type`、`read.character.is_active_player_group` | 已够用 |
| Game Host 兼容角色读面 | `extra.character.is_party_member`、`summoner`、`pool_slot`、`max_hp`、`hp` | 已够用；后续可迁至按域只读面后保持签名语义 |
| Game Host 角色写面 | `extra.character_write.max_hp`、`hp` | 已够用；仅 wrapper 在游戏线程调用 |
| Result | 所有上述函数的 `QolResult` 与 `kQolOk` | 已够用 |

本插件不使用 Host SDK 的 scheduler、commands、capabilities、store、panels、hooks、
text_scope、UI，也不使用 Game Host 的 target/thread、item、world、craft、save、quest、
choice、write/call 其它成员。当前行为所需 ABI 没有缺面；唯一显式延期项是
`JniBinding.signature_hash` 的实际哈希值，待 P5 三向 JNI 校验落地时补齐。

### 后续逐插件推进

每迁移一个插件，先把该插件现有实现拆入自己的独立 target，并将 target include 目录
限制为 `contracts/` 与该插件自身目录；再根据编译器暴露的真实调用点逐项增补
Host SDK、Game Host 或 provider capability 的契约面。每次只新增该插件实际需要的
POD/函数指针成员，不开放宿主根目录、不恢复 legacy include；target 能独立编译后，
再从 `gamebridge` 源列表移除对应 `.cpp` 并链接插件 target。行为面仍须使用受影响 VM
卡的真机日志验收，独立编译成功本身不替代行为证据。

### 构建结构：每插件 CMake 片段 + 自动发现

插件构建入口现收敛到以下结构：

```text
module/app/src/main/cpp/
  CMakeLists.txt                 # 定义 gamebridge，并 add_subdirectory(plugins)
  plugins/
    CMakeLists.txt               # 自动发现 */CMakeLists.txt，收口公共属性和链接
    simple_mode/
      CMakeLists.txt             # 只定义 qol_plugin_simple_mode 的源文件
      rules.cpp
      feature.cpp
      plugin.cpp
```

`plugins/CMakeLists.txt` 使用 `file(GLOB ... CONFIGURE_DEPENDS)` 发现
`plugins/<id>/CMakeLists.txt`，按目录名要求片段定义 `qol_plugin_<id>`。发现后统一设置
静态插件的 PIC、C++17、`-Wall -Wextra`，并且只向该 target 添加两个私有 include 目录：
`contracts/` 与 `plugins/<id>/`；随后把 target 以 `PRIVATE` 方式链接进已定义的
`gamebridge`。因此新增插件不需要修改父级源列表或链接列表。

新增插件的操作步骤：

1. 创建 `plugins/<id>/CMakeLists.txt`，只在其中定义 `qol_plugin_<id>`，源文件只列该
   插件目录内的实现文件；可参照 `simple_mode/CMakeLists.txt`。
2. 确认插件源文件只通过相对路径或 `contracts/`、自身目录头文件完成包含；不得依赖
   `game/`、`infra/`、`hooks/`、`patch/`、`host/` 或其它插件目录。父级不会向插件 target
   注入 `${CMAKE_CURRENT_SOURCE_DIR}` 根目录 include。
3. 从父级 `CMakeLists.txt` 的 `gamebridge` 源列表删除已经下沉的同一批 `.cpp`；未下沉
   插件继续保持原列表。自动发现只负责定义并链接 target，不会替父级去重。
4. 运行 host tests、Android debug 构建和独立 include 边界负例；确认目标的编译命令中
   没有宿主私有 include 路径，且同一 `.cpp` 只出现在插件 target 或 `gamebridge` 之一。

边界要求是结构约束而非约定：在 `plugins/<id>/` 临时加入
`#include "host/assembly.h"` 必须使该插件 target 编译失败，验证后移除；插件源文件不得
以扩大父级 include 路径或把宿主源文件加入插件 target 的方式绕过失败。此批只迁移
`simple_mode`，其它插件不创建 target，后续 lane 只需新增各自目录片段即可接入自动发现。
