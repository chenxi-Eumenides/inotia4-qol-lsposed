# `extension_bag` 插件参考

## 摘要

功能插件：提供 5 个扩展逻辑背包（袋号 `6..10`）、三宿主扩展页签（背包装备页/商店/合成器）、投影式拖放与存档 sidecar。对外发布 `extension_bag` 能力（API 3.0；3.1 尾追加 `remove_for_sale`）与 `inventory.merge_projection`；注册 8 条调试 HTTP 端点、3 个 port、1 个中立 inventory provider。

除特别说明，文中路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

- 插件 id `extension_bag`，版本 `0.1.0`（`plugins/extension_bag/plugin.cpp:49-50`）。
- required：`world.facts`、`item.facts`、`save.facts`、`craft.facts`、`save.slot_data`（`plugin.cpp:172-195`）。
- optional：`inventory.refresh`、`move_merge.control`、`http.registry`、`http.dispatch`（`plugin.cpp:197-208`）。
- provides：`extension_bag`（API major 3、minor 0；sale removal 3.1）、`inventory.merge_projection`（`plugin.cpp:155-170`；`contracts/extension_bag.h:25-30`）。
- ports：write port、character extras、inventory extras（`plugin.cpp:210-220,823-824`）。
- resources：`extension_bag_claims()` 声明表（`plugin.cpp:151-153,808-816`；`plugins/extension_bag/extension_bag_claims.inc:234`）。
- JNI：descriptor `jni_count=0`；两个导出由全局 bridge 管理（`plugin.cpp:852-880`；`bridge/native/gamebridge.cpp:226`；`bridge/native/gamebridge_domain_operations.cpp:458`）。
- feature：`extension_bag.extended_bag`，Gameplay/Toggle，`default_enabled=1`，显示优先级 -2，回调 `extension_bag_toggle`（`plugin.cpp:846-850`）。

## 公开能力与路由

### HTTP（8 条，owner 均为 `extension_bag`）

| 方法/路径 | 请求 | 语义 |
|---|---|---|
| `GET /api/debug/extension_bag/status` | 无 | 返回 enabled/injected/save_panel_hook/tab/frame/recovery_action/state |
| `POST .../enter_view` | `{bag:6..10}` | 进入扩展视图 |
| `POST .../exit_view` | 可省略 | 退回原版视图 |
| `POST .../select_bag` | `{bag:6..10}` | 扩展视图内切袋 |
| `POST .../unequip` | `{bag:6..10}` | 卸下扩展袋 |
| `POST .../click_item` | `{bag:6..10,slot:0..15}` | 选中/读取物品 |
| `POST .../equip` | `{index:0..4,bagType:1..4}` | 测试注入装备类型 |
| `POST .../item` | `{index:0..4,slot:0..15,category:N,count:N}` | 测试注入物品 |

- 声明与命令枚举：`plugin.cpp:226-278`；POST 只做字段校验并投递 typed command（handler id `0x45420001`），主线程执行后回写票据（`plugin.cpp:334-385,456-467`）。
- 成功 POST 返回 `{"ok":true,"ticket":N}`；业务结果仅在顶层 `ok` 为 `true` 时票据成功，原响应体保留。
- 参数错误返回 HTTP 400；字段缺失、非 int32、越界均拒绝。

### 能力与 port

- `ExtensionBagApi`：状态/索引、对象反查、投影、收编/消耗、交互接管、观测/拖放、移动、容量、jewel、`remove_for_sale`（`contracts/extension_bag.h:83-155`）。
- 线程约定：所有成员仅游戏主线程；`for_each_logical_item` 回调持 `g_virtual_bag_mtx`，回调内禁调本表加锁成员；撤销后不得缓存接口/裸指针（`contracts/extension_bag.h:9-15`）。
- write port 36 项：inventory 9、equipment 7、UI 20（`contracts/game_ports/extension_bag_write_port.h:11-70`）。`QolResult` 表示适配器调用状态；带 `out_result` 的函数由该输出承载游戏函数 `0` 成功/非 `0` 失败。
- 中立 provider：`inventory_extension_provider_table()`（`plugins/extension_bag/extension_bag_port.cpp:784`），消费者为 `core.item`、`inventory_hooks`、`core.save`。

## 数据模型

- 逻辑袋 `6..10`，内部索引 `0..4`；物理袋 `0..4` 普通、`5` 任务袋排除（`model/virtual_bag_state.h:63-73`）。
- 每袋固定 16 槽；容量按 BagType 派生：0→0、1→4、2→8、3→12、4→16（`virtual_bag_state.h:77`；`model/virtual_bag_transaction_rules.inc:88-90`）。
- `State`：5 袋类型、派生容量、5×16 槽物品、`mode`、选中/详情、`pending`、隔离记录（`virtual_bag_state.h:106-122`）。
- `Item`：category、count（canonical）、payload 长度与 256B 固定缓冲（`model/virtual_bag_transaction_types.h:3-8`）。
- pending 参与状态 JSON；`extensionbags.journal` v1 是模型/序列化/恢复判定，当前生产持久化只注册 `extensionbags.items` v4。
- ownership ledger：None/ModuleOwned/BorrowedForView/InventoryOwned/Released/ReservedForRelease，容量 128（`model/ownership_ledger.h:16-57`）。
- `ItemHandleCustody`：容量 1024，Empty/Active/Releasing（`model/item_handle_custody.h:22-27`）。

## 生命周期与持久化

- install：绑定 SDK/game/ports → require facts → 注册 typed result handler → inventory provider → lifecycle participant → `save.slot_data` owner → RenderPre flush（`plugin.cpp:547-756`）。
- activate/enable：关闭 gate 后用当前 owner token 重注册 slot_data；失败保持 gate 关闭；成功置 provider active + enabled（`plugin.cpp:799-816`）。
- deactivate/disable：注销 result handler/slot_data、清 cache、关 provider/enabled；provider/participant 为 set-once 常驻（`plugin.cpp:757-797`）。
- feature 回调：`extension_bag_toggle` → `set_virtual_bag_enabled`；只切运行时 gate 与 lifecycle task，资源安装/还原由宿主按 `want_run` 完成（`plugin.cpp:846-850`；`extension_bag_public_runtime.inc:415-435`）。
- 持久化：section `extensionbags.items` v4，SessionBound 读、SlotBound 写（`extension_bag_persistence.cpp:40-41,278-290`）。
- load：missing 清空状态并放行；非 v4、不可读、损坏 payload 阻断会话；读档保留 canonical，不按运行时上限截断。
- capture：锁内校验 + 序列化 + 容量检查，记录 transaction/epoch/revision；complete 仅在身份与 revision 匹配时清 pending/dirty（`extension_bag_persistence.cpp:196-255`）。
- dirty staging：每 revision 至多一次 `request_write`；失败清回 staged，安全点重试（`extension_bag_persistence.cpp:339-362`）。

## UI 宿主与输入

- 三宿主：背包装备页袋容器、商店袋列、合成器袋列；页签安装以 `g_virtual_bag_enabled` + session admission 为门控（`extension_bag_render.inc:1-20`；`extension_bag_store.inc`；`extension_bag_mix.inc`）。
- 页签控件 user type 2、data 16 字节、首字段指向物品对象；页签物品按类型复用/延迟释放（`extension_bag_runtime.inc:130-256`；`extension_bag_tabs.inc`）。
- 投影：draw wrapper 持锁、original-first；扩展视图下临时改原版当前袋为 sentinel，绘制后恢复（`extension_bag_render.inc:145-204,816`）。
- 输入：`0x02` 松开确认、`0x04` drop 到袋；moving 保留六条件；吞 release 后锁外 reset moving（`extension_bag_runtime.inc:13-108`；`extension_bag_input.inc:31-99`）。
- 详情按钮：经 Host SDK v1.27 `QolDescHookApi` 按 panel index `resolve` 出 button/data/execute-slot，按 execute-slot 去重后 `bind`；回调先 `validate` 绑定 id，身份不符直接 stale 返回；确认使用/出售/销毁在详情回调分流（`extension_bag_equip.inc:393-450,771-862,1139-1160`）。
- 几何：网格左上 `(761,160)`、4×4、单格 74×74、步距 83×83；页签矩形 57×57、纵向步距 70（`extension_bag_geometry.cpp:15-35`；`extension_bag_runtime.inc:119-129`）。

## 消费者与职责边界

| 消费者 | 入口 |
|---|---|
| `core.item` | 中立 inventory provider / `ExtensionBagApi` |
| `inventory_hooks` | 扩展物品识别、消耗、出售、移动 |
| `core.save` | `save.slot_data` v4 capture/complete |
| `bridge/native` | 两个全局 JNI 导出 |
| `move_merge` | 投影 drop / 身份查询 shim |

边界：物理袋、原版物品对象与存档位段归原版/Game Adapter；扩展状态、投影控件与 sidecar 归本插件；设置页渲染归宿主 feature catalog，插件只声明 feature。

## 文件地图

| 文件 | 职责 |
|---|---|
| `plugin.cpp/.h` | descriptor、HTTP 校验/票据、装配与生命周期 |
| `plugin.cpp` + `plugin.h` detail parser | 顶层 JSON int32/boolean 解析 |
| `routes.inc` | 8 条路由声明 |
| `extension_bag_api_impl.inc` | 调试能力实现 |
| `extension_bag_runtime.inc` | 页签/点击/事务恢复/状态操作 |
| `extension_bag_render.inc` | 投影安装/恢复、draw-end |
| `extension_bag_input.inc` | moving 清理与 release 等价清理 |
| `extension_bag_tabs.inc` | 页签物品对象缓存 |
| `extension_bag_transaction.inc` | ext↔orig、ext↔ext 事务 |
| `extension_bag_equip.inc` | 装备/卸下/详情/佣兵徽章 |
| `extension_bag_store.inc` | 商店页签、买入/卖出 |
| `extension_bag_mix.inc` | 合成器页签/投影 |
| `extension_bag_ownership.inc` | ledger/custody/延迟释放 |
| `extension_bag_persistence.cpp` | slot_data consumer |
| `model/*` | 纯状态、事务规则、JSON、payload、ledger |
| `extension_bag_port.cpp` | API 表、中立 provider、句柄 |
| `game_ui_virtbag.cpp` | 运行时共享状态、装备按钮、详情 hook 宿主 |

## Host 测试

- `extension_bag_contract_tests`：版本、POD/布局、枚举、patch site、JSON 参数/票据解析。
- `extension_bag_slot_data_tests`：注册、load 矩阵、canonical 保留、capture/complete、dirty staging、degraded。
- `extension_bag_sale_transaction_tests`：sale removal 模型、出售 plan、ext→orig 恢复判定。
- `extension_bag_draw_lock_tests`：draw 锁顺序、主菜单清理边沿、页签 generation/部分创建/投影安装模型。
- `extension_bag_move_merge_shim_tests`：provider active、身份查询、drop 结果映射。
- `extension_bag_status_query_tests`：status query 响应。
- 通用 `host_tests` 覆盖 ownership ledger、custody、事务阶段等纯模型。
