# ADR-0010：P2 Game Host face 拆分冻结

## 状态

已冻结目标面；P2-B4 已完成三类旧接口删除、使用者迁移和窄类型化写面收口。行为面仍
以受影响 VM 卡真机证据为准。

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
| `plugins/autosell/ui/panel.cpp` | `item_class.h`、`game/access/game_access.h`、`qol_log.h` | Host SDK UI/item read/log |
| `plugins/inventory_hooks/inventory_hook_stage4.cpp` | `sell_price.h`、`game_symbols.h` | inventory owner capability/command |
| `plugins/autosell/view.cpp` | `item_class.h`、`game/access/game_access.h` | Host SDK item read/provider capability |
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

P2-B1 的 `QolGameHostExtraApi`、`QolCallApi::call_known` 和 `QolUiPrimitiveApi` 仅作为
迁移期兼容面存在；P2-B4 已完成全部消费者迁移并删除，UI primitive 的替代面为
`QolUiAdapter`。

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

## P3：core.ui / save_backup 独立 target 收口增补

本批将两个插件的源码 include 面收口为 `contracts/` 与自身目录；宿主私有实现仍只在
宿主 adapter 内消费。新增或追加成员均为 append-only，不改变既有字段顺序、偏移或原
落点语义：

| 归属 | 成员签名 | 对齐原落点 |
|---|---|---|
| `QolGameHost::read_only.save` | `QolResult (*map_id)(int32_t slot, int32_t* out_map_id)` | `game/access/save::save_slot_map_id`，原 save backup UI 直接读取槽结构的 map 字段 |
| `QolGameHost` | `QolSaveCallApi save_call`，含 `free_data(void*)`、`encrypt(void*,int32_t,int32_t,const char*,int32_t*)`、`mark_stale()` | `game/access/save` 的存档内存释放、加解密和槽缓存失效原语 |
| `QolTargetApi` | `QolResult (*warehouse_companion_file_state)(QolTargetFeatureState*)` | `game_feature_state(kWarehouseCompanionFile)`；状态枚举逐值保持 unknown/unsupported/available/unavailable，save backup 仍仅对 unsupported 跳过伴生文件写回 |
| `QolUiAdapter` | `control_pressed(QolUiHandle,int32_t*)` | 原控件数据状态字段读取，供分页箭头保持按下态绘制 |
| `QolUiAdapter` | `touch_coordinates(uint64_t,int64_t*,int64_t*)` | 原 PopupState 触摸事件参数解码，插件不再读取控件/游戏结构偏移 |

此外，`QolHostSdk::result` 的 `write_ok(char*,uint32_t,uint32_t*)` 与
`write_error(const char*,char*,uint32_t,uint32_t*)` 作为插件响应信封面使用；其余 UI
运行时成员（主菜单按钮绑定、PopupState 栈顶判定、控件清理、screen name）继续使用
既有 `QolUiAdapter` 追加面。`QolGameHost` minor 更新为 6，`QolHostSdk` minor 保持
3，UI adapter minor 保持 3。

save backup 不再依赖 `game_feature.h`：伴生文件门禁通过 target 面取得同一功能状态；
除 `unsupported` 分支外，unknown/available/unavailable 的写回路径保持原判定。两个
插件的 JSON 成功/错误信封均经 Host SDK result 面生成，不再 include `game_ops_common.h`。

## P2-ABI-1：按需增补 autosell 只读面

本批只在既有 `read_only` 子表尾部追加成员，`QolGameHost` 的既有成员顺序不变，
`kGameHostMinor` 从 3 递增为 4。宝石位域、库存遍历/引用和 `g_gamestate` 门禁均由
`game/access/*` 适配，插件 ABI 不暴露 `g_base`、裸偏移或 `game_symbols.h`。

| 域 | 成员 / 签名 | 语义与原落点 |
|---|---|---|
| item | `jewel_value(const void*, int32_t*)` | 读取宝石 `item+0x10` bits0–10；原落点 `plugins/autosell/view.cpp:77`，原语 `game/access/item_class.cpp`。 |
| item | `jewel_attribute_type(const void*, int32_t*)` | 读取宝石 `item+0x10` bits18–23；原落点 `plugins/autosell/view.cpp:79`，原语 `game/access/item_class.cpp`。 |
| world | `game_state_gate(int32_t*)` | 读取 `g_gamestate` 并输出 `1` 当且仅当原值为 0；`g_gamestate` 空指针按原实现取 0。与原扫描门禁 `world.state == 5` 之外的独立判定逐字等价，原落点 `plugins/autosell/scan.cpp:64-75`。 |
| inventory | `item_ref_at(int32_t, int32_t, QolInventoryItemRef*)` | 保留 `InventoryItemRef` 的 kind/bag/slot/category/count/native_item 语义；空槽为 `not_found`，物理库存未就绪为 `not_ready`。原落点 `game/state/game_state.cpp:215-250`、`plugins/autosell/scan.cpp:155`。 |
| inventory | `for_each_bag_slot(QolInventoryBagSlotFn, void*)` | 聚合遍历物理袋 `0..5`，回调返回 1 停止；引用仅在回调期间有效。原落点 `game/state/game_state.cpp:280-292`、`plugins/autosell/scan.cpp:119-145`。 |

本批不迁移 `autosell` 消费者。缺面映射为：宝石属性值 → `read_only.item.jewel_value`；
宝石属性类型 → `read_only.item.jewel_attribute_type`；库存槽遍历 →
`read_only.inventory.for_each_bag_slot`；库存引用 → `read_only.inventory.item_ref_at`；
in-world 的 `g_gamestate == 0` 门禁 → `read_only.world.game_state_gate`。出售事务
（金额、扣减、扩展袋 owner）和 UI 控件身份面仍未覆盖，留给后续 lane。

## G1：扩展背包宿主设施面增补

G1 将 `kHostSdkMinor` 从 3 增至 4，并在既有 `QolStoreApi` 尾部追加存档参与者注册和存档触发成员，不改变已有成员顺序、
偏移或签名：

- `register_participant(const QolStoreParticipant*)`：把插件的 prepare/commit/abort
  回调转发到宿主存档参与者登记表；参数和宿主未就绪均返回 `QolResult` 错误。
- `save_game()`：把扩展背包保存入口转发到宿主模块存档编排；宿主未提供该入口时返回
  `kQolErrNotReady`。

扩展背包日志经自身薄封装调用 `QolHostSdk::log.write`，配置变更经 Host SDK 事件订阅，
能力查询仍按每次调用取得接口，未缓存跨撤销事务的裸指针。调度、资源声明和能力发布
沿用既有宿主管理路径；本组不改变 29 条资源声明、投影/事务/回滚或锁纪律。

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
    push_all/
      CMakeLists.txt             # 只定义 qol_plugin_push_all 的源文件
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
3. 不再从父级 `CMakeLists.txt` 的 `gamebridge` 源列表删除已经下沉的 `.cpp`；片段被发现
   后，父级会按插件目录前缀自动过滤该批源。未下沉插件继续保持原列表。
4. 运行 host tests、Android debug 构建和独立 include 边界负例；确认目标的编译命令中
   没有宿主私有 include 路径，且同一 `.cpp` 只出现在插件 target 或 `gamebridge` 之一。

边界要求是结构约束而非约定：在 `plugins/<id>/` 临时加入
`#include "host/assembly.h"` 必须使该插件 target 编译失败，验证后移除；插件源文件不得
以扩大父级 include 路径或把宿主源文件加入插件 target 的方式绕过失败。此批只迁移
`simple_mode`，并以 `push_all` 验证片段发现后的自动摘除；其它插件后续只需新增各自目录
片段即可接入自动发现。

### 并行迁移前置：自动摘除机制

为避免并行 lane 在迁移同一插件时同时编辑根级源列表，根
`CMakeLists.txt` 只维护一次 `QOL_BRIDGE_PLUGIN_SOURCES` 集中清单。插件父级在
`add_subdirectory(plugins)` 配置各插件片段后，按已发现的 `plugins/<id>/` 路径前缀从
该清单中过滤对应源文件，再把剩余源加入 `gamebridge`。因此独立插件 target 与
`gamebridge` 不会重复编译同一插件源；过滤使用路径前缀而不是插件名正则，避免插件 id
中的正则字符改变匹配含义。

该机制选择“根级集中清单 + 父级按已发现 id 过滤”，因为现有根 target 仍需保留尚未
迁移插件的直接源，且新增片段只需改变父级的配置期发现结果，不需要再次编辑根文件。
`file(GLOB ... CONFIGURE_DEPENDS)` 会在新增或删除片段后触发重新配置；仅改变片段内容
仍按普通 CMake 配置依赖处理。

新增插件只需两步：

1. 创建 `plugins/<id>/CMakeLists.txt`，定义 `qol_plugin_<id>`，列出该插件自己的源；
   父级会自动发现片段、设置公共 target 属性并链接到 `gamebridge`，同时自动摘除集中清单
   中 `plugins/<id>/` 前缀的源。
2. 修越界：根据独立 target 的编译错误，补齐 `contracts/`、自身目录或已冻结的 Host
   SDK/Game Host API 契约，消除对宿主私有目录和其它插件源码的 include；不再修改根
   `CMakeLists.txt` 的插件源条目。

`push_all` 作为本机制的试点：新增片段后其三个源只属于
`qol_plugin_push_all`，根 `gamebridge` 保留 JNI 薄层和 patch 机制源但不再直接编译
`plugins/push_all/*.cpp`。本试点只改变编译归属与链接方式，不改变装配顺序、描述符或
运行时逻辑；行为面仍按既有 VM-P4-01 真机证据约束，不以构建结果替代真机验收。

## EB G2：只读事实迁移增补

G2 在现有 `read_only` 子表尾部追加 Character、Item、Save 和 Inventory 只读成员，
`kGameHostMinor` 从 7 递增为 8；既有字段顺序、偏移和签名保持不变。新增成员包括：

- Character：`can_equip`、`find_equip_slot`、`equipped_item`、`has_empty_mercenary_slot`；
- Item：`sell_price`、`is_enchant_scroll`、`is_mercenary_seal`、`is_usable`；
- Save：`current_slot`、`is_ok`；
- Inventory：`bag_size`、`is_empty_bag`、`find_save_slot`。

实现路径统一为 `host/game_host.cpp` → `game/access/*` → legacy game symbol，插件只经
`QolGameHost::read_only` 查询。EB 已将 world gate、物品类别/数量、装备判定、佣兵徽章、
可用物品、出售单价和当前存档槽的对应只读调用切换到该面；原有投影、事务、回滚、
marker 与 owner 写路径不在本组改变。`PtrHook`、UI 控件身份、`g_base`/`g_inven` 裸布局
以及副作用调用仍属于 G3/G4/S1 缺口，不以只读面包装。

Host fake 覆盖新增表项的正常值、空参数和未就绪语义；本组不执行真机安装和行为回归，
因此受影响规则 R-47、R-50、R-55、R-63 的 VM-43/VM-P4-07 证据仍为 `NOT_ACCEPTED`。

## EB G3：UI adapter 迁移增补

G3 仅在 `QolUiAdapter` 尾部追加 EB 所需 UI 原语，`kUiAdapterMinor` 从 3 递增为 4；
既有成员顺序、偏移和语义保持不变。新增成员包括：

- `control_user_type`、`control_item`、`control_item_slot`：读取 EB 控件身份、物品和槽位；
- `touch_state`：读取 moving/drop source 与 release 坐标；
- `fill_rect`、`draw_item_porting`：转发原版高亮和物品绘制；
- `control_parent`、`find_root`：查询父链和装备/商店/合成器控件根。

EB 的几何、控件树、命中、页签和格子绘制路径经 `extension_bag_geometry` 薄封装调用
`ctx.sdk->ui`；插件不直接读取 `CO_*`、`TOUCH_STATE_*`、UI root VMA 或图像句柄。原有
投影、事务、回滚、owner 写路径不在本组改变。Popup enter alias 接线、fake adapter 测试和
真机 VM-B 证据仍未完成，因此本组状态为 `NOT_ACCEPTED`。

## EB G4：副作用与所有权写面增补

G4 在 `QolGameHost` 尾部追加 `extension_bag_write`，`kGameHostMinor` 从 8 递增为 9；
既有成员顺序、偏移和签名保持不变。新增面按 EB 自身写语义分组：

- `inventory`：`save_item_on_empty`、`save_item`、`serialize_item`、`load_item`、
  `create_item`、`free_item`、`remove_item_direct`、`move_item`；
- `equipment`：装备交换、镶嵌、物品使用、佣兵生成及金币加减；
- `ui`：装备/背包/商店/合成刷新、装备状态更新和详情关闭。

每个函数均返回 `QolResult`，原游戏布尔/整数返回值通过 `out_result` 保留；未解析、空对象、
越界和失败路径不转换为成功。实现路径为 `host/game_host.cpp` → legacy game symbol，
插件只读取 `QolGameHost::extension_bag_write`。EB 的装备、跨袋入库、移动合并、镶嵌、
物品使用、佣兵生成、出售金币、序列化/重建和对象释放路径已切换到该窄面；原版刷新
trampoline、Popup/PtrHook 安装和控件树写仍登记为 G5/S1 缺口。

`test_host_adapters.cpp` 新增 ABI 成员、正常入口可用性和参数拒绝断言；host tests 通过。
受影响规则为 R-12、R-18、R-34、R-38、R-40、R-41、R-47、R-48、R-49、R-50、R-55、R-63，
对应 VM-B01～VM-B04、VM-B05、VM-34、VM-40、VM-41。未执行真机回归，因此行为验收仍为
`NOT_ACCEPTED`。

## P2-B4：旧接口删除与窄面迁移

本批删除三类旧接口，不保留兼容 shim：

1. 删除 `QolGameHostExtraApi` 及其跨域聚合类型。只读成员统一进入既有
   `QolGameHost::read_only`；副作用成员拆为 `character_write`、`inventory_write`、
   `craft_action`、`world_action`、`quest_write`、`choice_action` 六个按域函数表，存档
   操作使用既有顶层 `save_call`。例外是 `QolInventoryReadApi::bit_range`：该成员在删除
   前已无任何使用者（仅声明和装配，无调用点），未带入 `read_only`，随聚合类型一并删除。
2. 删除 `QolCallApi::call_known` 及 `QolCallApi`。没有使用者继续通过 ID、参数 blob 或
   任意函数名调用游戏函数。
3. 删除 `QolUiPrimitiveApi` 及 `QolGameHost::ui_primitive`。内联属性颜色写入迁移到
   `QolHostSdk::ui` 的 `QolUiAdapter::insert_inline_color`，UI adapter minor 从 4 升为 5。
   该成员仍由 host adapter 执行边界检查和 `game/access/attr_range_ui` 原语调用。
   adapter 的 `not_ready`/`unsupported` 诊断文本随接口名从 `ui primitive …` 改为
   `ui adapter …`；错误码不变，`message_len` 与字面量字节数一致（由 `ui_adapter_tests`
   断言 `message_len == strlen(message)` 固定）。

### 使用者迁移落点与等价性

| 使用者 | 原接口成员 | 新面 | 语义等价与边界 |
|---|---|---|---|
| `plugins/custom_recipe/feature.cpp` | item/craft 读取、物品创建/保存/释放、描述关闭 | `read_only.item`、`read_only.craft`、`inventory_write`、`craft_action` | 原 `game/access` 落点和调用顺序不变；空参数、未解析、创建失败仍返回原 `invalid_argument`/`not_ready`，创建失败仍清理对象 |
| `plugins/craft_ui/feature.cpp`、`plugin.cpp` | craft 读取/调用、item 读取 | `read_only.craft`、`craft_action`、`read_only.item` | 仍按原 slot/type 边界检查；调用失败仍被忽略或按原 guard 短路 |
| `plugins/world_teleport/feature.cpp` | world 读取/切图/扣钱/弹窗/HUD/banner、choice state | `read_only.world`、`world_action`、`read_only.choice` | map id、金额、文本和回调参数校验保持；游戏原函数返回失败仍映射为 `not_ready`，不伪造成功 |
| `plugins/inventory_hooks/plugin.cpp`、`native_inventory_hook.cpp` | 出售结算的加钱/扣钱/移除物品 | `world_action.add_money/minus_money`、`inventory_write.remove_item/remove_item_direct` | 原子顺序和失败补偿不变；任一原语未就绪仍停止事务并执行既有回滚 |
| `plugins/quest/feature.cpp` | 任务 active/state/complete 读取、状态写入 | `read_only.quest`、`quest_write` | `quest_state(quest_id)` 保留按任务 id 查询；写入仍只在主线程调度中执行，失败不增加修复计数 |
| `plugins/native_choice/feature.cpp` | choice 读取、写选项/标题/焦点、打开/关闭/音效 | `read_only.choice`、`choice_action` | 原 count/index/state 边界检查和 popup 调用顺序不变；原函数失败仍返回 `not_ready` |
| `plugins/special_equip/feature.cpp` | character/item 读取 | `read_only.character`、`read_only.item` | 只读值和空对象/未解析错误保持，不再经过聚合表 |
| `plugins/simple_mode/feature.cpp` | character 读取和 HP 写入 | `read_only.character`、`character_write` | 原 HP 非负校验、主线程 wrapper 和失败返回保持 |
| `plugins/attribute_range_display/feature.cpp` | `ui_primitive.insert_inline_color` | `sdk->ui->insert_inline_color` | 同一 4096 字节上限、空参数和颜色码校验；超界仍为 `blocked`，原颜色原语失败仍为 `not_ready` |
| `tests/test_host_adapters.cpp` | legacy extra、`call_known`、UI primitive 断言 | read-only、窄写面、唯一 UI adapter | fake/host 测试改测新函数表与错误码，不再为旧成员保留断言 |
| `tests/test_quest_lifecycle.cpp` | fake quest extra | `read_only.quest.quest_state`、`quest_write` | fake 保持相同 quest id、状态和调用次数断言 |

### 新增窄写原语

以下函数表由 `host/game_host.cpp` 实现，实际游戏调用仍统一落到 `game/access/*` 的已解析
原语；契约只暴露包装器函数指针，不暴露裸游戏函数指针、地址或 `PtrHook`：

| 面 | 签名（成员） | 语义与失败返回 |
|---|---|---|
| `QolCharacterWriteApi` | `max_hp(void*, int32_t)`、`hp(void*, int32_t)` | 写角色最大/当前 HP；空对象或负值为 `invalid_argument`，目标原语不可用为 `not_ready` |
| `QolInventoryWriteApi` | `remove_item_data(int32_t,int32_t)`、`remove_item(void*)`、`create_perfect_item(int32_t,void**)`、`create_item(int32_t,int32_t,int32_t,int32_t,void**)`、`save_item(void*,void*,int32_t*)`、`free_item(void*)`、`remove_item_direct(int32_t,int32_t)` | 对应原版库存删除、创建、保存、释放和按槽移除；参数越界为 `invalid_argument`，函数未解析为 `not_ready`，创建/删除原函数失败不转成功 |
| `QolCraftActionApi` | `init_mixing_state()`、`reset_stuff_item_control()`、`refresh_inventory_items()`、`select_first_empty_stuff_slot(int32_t*)`、`set_ui_type(int64_t)`、`set_selected_recipe(int32_t,int64_t)`、`play_sound(int32_t)`、`show_text(uint32_t)`、`show_yesno_from_text(uint32_t,void*,void*)`、`description_set_off()` | 合成状态、选择、刷新、音效、文本和原生确认框动作；type/sound/text/callback 参数越界为 `invalid_argument`，落点不可用为 `not_ready` |
| `QolWorldActionApi` | `create_yesno(const char*,uint32_t,void*,void*,void*)`、`minus_money(int64_t)`、`request_map_change(int32_t,int32_t,int32_t,int32_t)`、`close_popup()`、`restore_hud()`、`show_banner(const char*)`、`add_money(int64_t)` | 世界切图、扣/加钱、原生选择框和 HUD 动作；金额/map/text 边界保持，原函数失败为 `not_ready`，不写默认值 |
| `QolSaveCallApi` | `free_data(void*)`、`encrypt(void*,int32_t,int32_t,const char*,int32_t*)`、`mark_stale()` | 存档数据释放、加解密和缓存失效；size/mode/key/out 参数非法为 `invalid_argument`，加解密依赖未就绪为 `not_ready` |
| `QolQuestWriteApi` | `set_state(uint32_t,int32_t)` | 写指定任务状态；quest id 超范围为 `invalid_argument`，游戏原语拒绝为 `not_ready` |
| `QolChoiceActionApi` | `write_items(const char* const*,uint32_t)`、`write_title(const char*)`、`write_focus(int32_t)`、`push()`、`close_popup()`、`play_sound(int32_t)` | 原生选择框内容、焦点、打开/关闭和音效；count/index/空字符串边界为 `invalid_argument`，原函数失败为 `not_ready` |

所有只读成员的落点保持 `host/game_host.cpp → game/access/*`，错误时不向 out 写默认值；
存档释放、加解密、选择框和世界动作也沿用原调用窗与主线程约束。扩展背包已有
`extension_bag_write` 面未改动，锁纪律、事务语义和同调用窗同步写回不变。
