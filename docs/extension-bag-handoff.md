# 扩展背包交接文档

> 状态：开发中，尚未完成真机验收。本文档是后续继续开发的唯一交接基线。

## 1. 功能范围

扩展背包是在原版装备/背包页面中增加的 5 个扩展背包入口。当前规格固定为：

| 扩展背包 | 容量 |
|---|---:|
| 1 | 16 格 |
| 2 | 8 格 |
| 3 | 4 格 |
| 4 | 0 格 |
| 5 | 0 格 |

当前阶段不实现：

- 将原版背包物品装备到扩展背包。
- 扩展背包物品信息展示。
- 扩展背包边框、图标和选中视觉的最终重做。

## 2. 原版逻辑（已通过反汇编确认）

### 2.1 当前原版背包

原版维护两个同步值：

- direct：`G_UIEQUIP_CUR_BAG_VMA = 0x304a41`，面板结构中的字节。
- GOT：`G_UIEQUIP_CUR_BAG_GOT_VMA = 0x2f56d8`，指向另一份当前袋索引。

原版正常路径会同时写两份值。渲染函数主要读取 GOT 目标，部分业务逻辑读取 direct。

### 2.2 原版容量

`INVEN_GetBagSize(0x103250)` 的数据链：

```text
G_BAG_TABLE_VMA 0x2f3bc0
  -> 袋结构指针数组[bag]
  -> 袋结构 +0x10 的 u32
  -> 低 25 位
```

容量不是固定 16。原版 `UIEquip_RefreshItemArea(0xb7a00)` 读取当前袋容量后遍历 16 个槽：

- `slot < capacity`：`SetActive(1)`、`SetShow(1)`、绑定物品。
- `slot >= capacity`：`SetActive(0)`、`SetShow(0)`。

`UIEquip_DrawInvenItem(0xb6fac)` 每帧再次读取同一个容量，只绘制容量范围内的格子。

### 2.3 原版袋页签事件

场景事件 `Scene_Event_POPUP_SC_EQUIP(0x14acd0)` 将触摸事件分发给控件树：

- `0x17`：触摸按下，选择控件。
- `0x18`：触摸释放，触发控件动作。

原版袋控件处理器 `UIEquip_InvenBagControlEventProc(0xb89d0)` 的 `event=0x2` 才真正切换袋：

1. 根据控件索引得到目标袋。
2. 容量为 0 时不切换。
3. 目标袋与当前袋不同：同时写 direct/GOT，调用 `UIEquip_RefreshItemArea`。
4. 目标袋与当前袋相同时，进入原版描述逻辑。

原版高亮由 `UIEquip_DrawInvenBag(0xb7284)` 每帧比较：

```text
bag_index == GOT 当前袋索引
```

相等时绘制选中框/高亮贴图。

## 3. 当前实现逻辑

### 3.1 扩展背包状态

文件：`module/app/src/main/cpp/virtual_bag_state.h`

当前状态包含：

- 5 个扩展背包。
- 每个扩展背包 16 个逻辑物品槽。
- `types`、固定 `capacities`、`items`。
- `mode`、`selected`、`inspected`、`original_selected`。

容量只由固定数组 `{16, 8, 4, 0, 0}` 定义；`normalize()` 每次都会恢复该固定规格。按背包类型推导容量的旧逻辑已删除。

### 3.2 物品投影

文件：`module/app/src/main/cpp/game_ui_virtbag.cpp`

`install_module_view_locked(index)` 当前把扩展背包物品临时写入原版当前袋的 `g_inven` 槽区：

```text
原版当前袋 * 16 + 扩展背包槽
```

同时保存：

- 原版 96 个物品指针。
- 原版 direct/GOT 当前袋。
- 原版袋容量字段。

`restore_module_view_locked()` 恢复上述快照，并刷新原版物品区。

### 3.3 容量投影

当前扩展视图期间持续改写原版当前袋对象的 `+0x10` 低 25 位，使原版 `RefreshItemArea` 和每帧 `DrawInvenItem` 读取到扩展容量。退出时恢复原值。

这是当前唯一应保留的容量方案。早期“只在刷新调用期间改容量、随后立即恢复”的方案已被证明无效，不应恢复。

### 3.4 绘制钩子

当前有两个绘制相关钩子：

1. `UIEquip_Draw` 内对 `UIEquip_DrawInvenBag` 的调用包装：扩展视图期间临时把 GOT 当前袋设为 `6`，阻止原版袋高亮。
2. `Scene_Draw_POPUP_SC_EQUIP + 0x210` 的 `GRPX_End` 调用包装：每帧安装/恢复扩展物品投影，并绘制扩展入口。

两个绘制钩子只根据已提交的运行时状态决定是否安装或遮蔽投影；退出中的状态不会重新安装扩展投影。

### 3.5 事件钩子

模块替换的是场景层事件回调，不是原版袋控件处理器。

当前扩展入口点击由模块直接处理。原版袋点击会在 `0x17` 恢复扩展投影、进入退出状态并将 direct/GOT 设为无效选择；`0x18` 由原版控件完成实际袋切换后，模块才以 GOT 提交原版状态。该过程不按坐标预写目标袋。

## 4. 已实现内容

- 扩展背包状态持久化到 `ModuleSaveStore`。
- 5 个扩展入口的点击状态机。
- 固定容量 `16/8/4/0/0`。
- 扩展物品投影到原版槽区。
- 扩展容量持续覆盖原版容量字段。
- 扩展视图期间抑制原版袋选中框。
- 退出扩展视图时恢复原版库存、容量和当前袋快照。
- 扩展入口触摸事件与原版场景事件分流。
- 退出扩展视图时使用 `ExitingModule` 覆盖完整的 `0x17→0x18` 触摸周期，原版释放事件完成后以 GOT 当前袋为权威同步 direct/GOT，再提交原版状态。
- 删除未安装的“使用背包物品装备扩展背包”hook 脚手架。
- host tests、CTest、Gradle APK 构建均可通过。

## 5. 当前未解决问题

### 5.1 从扩展背包切回原版后高亮延迟

现象：

1. 扩展背包切到原版背包后，目标原版背包不立即高亮。
2. 再点击其他原版背包时，前一个背包短暂高亮，然后当前背包高亮。

已完成的修复：

- 新增 `ExitingModule` 运行时过渡状态，投影恢复后不会再以旧 `Module` 状态重装。
- 原版事件返回后以 GOT 当前袋为高亮权威值，并同步写回 direct/GOT。
- 进入扩展视图前也会统一已有的 direct/GOT 差异，确保物品投影与容量字属于同一原版袋。

仍需真机验证以下运行时观测：

```text
direct 当前袋
GOT 当前袋
g_original_current_direct
g_original_current_got
mode
g_module_view_installed
```

使用真实触摸复现扩展袋→任意原版袋、同袋返回、快速连续点击与关闭页面四条路径，确认首帧只高亮目标原版袋。

### 5.2 遗留冗余代码

已清理：

- `kUseItemCallOffsets`、`g_use_item_thunk`、`g_use_item_patch_installed`。
- `virtual_bag_char_use_item_ex()`、`virtual_bag_equip_selected()`、`virtual_bag_can_equip_selected()` 及仅供它们使用的符号。
- `capacity_for_type()` 和 `test_host.cpp` 中重复的 `state = {};`。

仍可在后续最小化持久化格式时评估：

- 仅写入、不参与当前业务决策的 `original_selected`。

不应清理：

- 原版库存快照与恢复。
- 原版容量字段快照与恢复。
- direct/GOT 双写本身；这是原版确实存在的双状态。
- 原版事件委托；它是恢复原版控件焦点和切换语义所必需的。

## 6. 需要实现的目标逻辑

### 6.1 功能目标

- 5 个扩展背包稳定显示并可切换。
- 容量固定为 `16/8/4/0/0`。
- 扩展物品槽数量始终与扩展容量一致。
- 扩展视图期间原版背包不出现选中高亮。
- 扩展背包可直接切回任意可用原版背包。
- 切回原版后目标袋在同一显示周期立即高亮。
- 原版袋 0/空袋边界遵循原版容量校验。
- 关闭背包页面、切换存档、快速连续点击后状态不残留。

### 6.2 架构目标

- 持久化只保存 `types/items` 或固定规格所需的最小数据。
- `mode/selected/inspected` 作为运行时 UI 状态，不写入过期展示状态。
- 由 `Original`、`Module`、`ExitingModule` 三态管理投影安装、恢复、事件委托和绘制。
- 绘制钩子只执行已提交状态，不负责决定业务模式。
- 原版事件完成后读取原版实际 current，不再按触摸坐标强写结果。

## 7. 推荐后续实施顺序

1. 完成原版切回、扩展切换、关闭页面、空袋点击四组真机回归。
2. 如果高亮仍异常，记录 direct/GOT/mode/overlay 四值，再根据实测调整；不得重新引入按坐标预写目标袋的补丁。
3. 将运行时 UI 状态从持久化 payload 中移除，只保存固定规格所需的物品数据。
4. 处理扩展物品对象缓存的生命周期，避免类别变化时遗留对象。

## 8. 关键文件

| 文件 | 职责 |
|---|---|
| `module/app/src/main/cpp/game_ui_virtbag.cpp` | 扩展背包投影、事件、绘制和持久化接线 |
| `module/app/src/main/cpp/virtual_bag_state.h` | 扩展背包运行时状态机 |
| `module/app/src/main/cpp/game_ui_virtbag.h` | C++ 公共接口 |
| `module/app/src/main/cpp/game_symbols.h` | 原版 UI/背包符号与地址 |
| `module/app/src/main/cpp/symbol_registry.h` | 动态符号注册 |
| `module/app/src/main/cpp/game_inventory.cpp` | 原版背包 API 操作 |
| `module/app/src/main/java/com/inotia4/export/ExtensionBagUiBridge.kt` | 状态持久化桥接 |
| `module/app/src/main/cpp/tests/test_host.cpp` | 状态机 host 回归测试 |

## 9. 术语约定

用户可见名称统一使用：**扩展背包**。

现有 `virtual_bag`、`VirtualBag`、`virtbag` 等名称属于历史内部标识。后续新增代码和文档不得继续使用这些名称；若重命名已有 C++/JNI/API 标识，必须一次性完成全链路迁移，不能只改一层。
