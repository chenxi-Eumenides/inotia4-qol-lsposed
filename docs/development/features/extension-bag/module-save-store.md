# 模块存档容器与保存协调

## 目标与边界

每个原版存档槽 `0..2` 对应一个模块 sidecar。模块新增的持久化数据必须放入具名 section；原版 `save*.dat` 不读取、不写入、不追加且不重命名。纯内存补丁（例如堆叠上限）不使用该容器。

v1 只编排模块 API 驱动的存档生命周期：进入槽时加载一次，新建槽成功时清空对应容器；功能 section 的内存改动只在确认原版完整存档成功后写入 sidecar。退出背包、切换界面、恢复投影和普通内存刷新都不是保存事件。退出到主菜单、切换存档或进程结束前未成功保存的改动全部丢弃，不依赖退出回调持久化。

## 保存协调边界（P6）

`ModuleSaveStore` 不负责推断功能语义，也不把任意 native 子函数调用视为保存成功。保存协调器必须区分以下事件：

| 事件 | 是否提交模块 section | 说明 |
|---|---|---|
| 退出背包/切换袋/恢复投影 | 否 | 只恢复 UI、native 借用对象和运行时快照 |
| 模块功能操作（例如批量宝石合成） | 否 | 只修改游戏内存；不隐式调用原版保存，也不提交模块 section |
| `SAVE_SaveInventory` 子调用 | 否 | 这是完整 `SAVE_Save` 的内部步骤，只能执行保存前投影恢复和准备动作 |
| 完整原版 `SAVE_Save` 返回成功 | 是 | 仅此时允许提交已准备的模块快照 |
| 完整原版保存返回失败或结果未知 | 否 | 保留 prepare journal，禁止标记 committed |

模块存档层提供通用的保存协调生命周期：

```text
participant snapshot
→ 写入协调 journal
→ 调用并确认原版完整保存
→ 提交各 participant section
→ 清理协调 journal
```

扩展背包是第一个 participant，负责 `extensionbags.items`、payload 和业务恢复判断；其他模块只需注册自己的 section、快照和恢复处理。模块存档可以自动提供槽隔离、原子写入、CRC、last-good、generation 和统一提交时序，但不能自动推断业务数据如何重放或回滚。

## 原版保存入口审计

当前游戏版本的 `SAVE_Save` 静态调用点已逐一登记。下列调用点代表完整保存请求，统一改写为调用 core 的 `module_save_game()`；该函数内部再调用原始 `SAVE_Save`，因此不会绕过 participant 的 prepare/commit：

| 调用点 | 原调用者 | 语义 | 处理 |
|---|---|---|---|
| `0xc4488` | `UINpcRevive_Revive_Confirm` | 复活确认后的完整落盘保存 | 统一协调 |
| `0xcbea0`, `0xcbf4c` | `UIQuestMenu_ClearUIInAppProcess` | 任务菜单流程完成后的完整落盘保存 | 统一协调 |
| `0x125c88` | `QUESTSYSTEM_AcceptReivew` | 接受任务后的完整落盘保存 | 统一协调 |
| `0x129850` | `SAVE_ProcessSave` | 原版保存状态机的完整落盘保存 | 统一协调 |
| `0x15d708` | `NetworkStore_AddItem` | 网络商店物品处理后的完整落盘保存 | 统一协调 |
| `0x15da84`, `0x15dcf0` | `NetworkStore_Process` | 网络商店流程后的完整落盘保存 | 统一协调 |
| `0x129f24` | `SAVE_LoadCharacterAll` | 读档内部的特殊保存调用 | 保留原版，避免读档流程重入协调器 |

`SAVE_SaveInventory`（`0x127d8c`）是 `SAVE_Save` 内部序列化背包的子步骤，只能恢复投影；`SAVE_SaveItem`、`INVEN_SaveItem*` 只修改内存或构造序列化对象。它们都不是独立的模块存档提交时机。协调器带有线程局部重入门禁，避免保存面板或原版保存状态机重复触发二次完整保存。

## Journal 分层

- **协调 journal**：由模块存档层记录一次“原版保存与多个模块 section 提交”的全局阶段、槽位、generation、参与 section 和原版保存结果。它不保存 native 指针，也不解释扩展背包槽位语义。
- **功能 journal**：由扩展背包记录移动事务的 source/target、payload、方向和业务阶段，用于判断物品应重放、回滚或隔离。它存在的理由是保证跨原版/扩展背包操作的正确性，不应被通用容器替代。

两者不得互相冒充：协调 journal 解决“保存提交是否完成”，功能 journal 解决“某笔背包事务如何恢复”。如果只有一个 participant，仍保留协调边界，避免以后新增模块时再次改写保存入口；可以在实现中将协调 envelope 与 participant journal 原子写入同一容器，但语义必须分开。

## 文件与恢复

文件根目录为 `context.getExternalFilesDir(null)/module-saves/`：

| 文件 | 用途 |
|---|---|
| `slot-{n}.module-save` | 当前 sidecar |
| `slot-{n}.module-save.last-good` | 上次有效主文件快照 |
| `*.corrupt.<timestamp>` | 校验失败后保留的诊断副本 |

写入使用 `android.util.AtomicFile`。更新主文件前，当前有效主文件会原子写入 `last-good`；随后原子替换主文件。加载主文件失败时隔离它并尝试 `last-good`；两者都无效时以空容器继续，记录日志，且绝不影响原版游戏存档。

## 二进制容器格式（v1）

容器按大端 `DataOutputStream` 编码：

```text
u32 magic = "MSAV"
u16 formatVersion = 1
u8  slotId
u64 generation
u16 sectionCount
repeat sectionCount:
  u8  nameLength
  u8[nameLength] ASCII sectionName
  u16 sectionVersion
  u32 payloadLength
  u8[payloadLength] payload
  u32 payloadCrc32
u32 containerCrc32  // 覆盖之前全部字节
```

section 名只能是 `[a-z0-9._-]{1,64}`，单个 payload 最大 1 MiB，整个容器最大 4 MiB。未知 section 始终以原 payload 和版本保留；只有调用方显式 `removeSection` 才删除。

## 组件 API

`ModuleSaveStore` 是内部 Kotlin 单例，所有操作经同一把锁串行化：

- `ensureSlot(slot)`：确保容器存在。
- `readSection(slot, name)`：读取防御性副本。
- `writeSection(slot, name, version, payload)`：原子替换一个 section。
- `removeSection(slot, name)`：显式删除一个 section。
- `resetSlot(slot)`：创建新原版存档时清除旧 sidecar 与 last-good。

保存协调器还必须提供等价的内部能力（不作为 HTTP/JNI 公共 API）：

- `prepare(slot, participants)`：生成不可变 participant 快照并落盘协调 journal；不能因退出页面或 UI restore 调用。
- `commitAfterOriginalSave(slot, result)`：仅在完整原版保存返回明确成功后提交快照；失败或结果未知时保留 journal。
- `recover(slot, originalSaveProbe)`：启动/进档时先读取协调 journal，再由各 participant 根据原版实态决定重放、回滚或隔离。

section payload 对容器不透明。扩展背包等未来功能自行定义 payload 数据结构与版本，并通过这个 API 持久化；v1 不提供 HTTP、JNI 或原版 UI 回调入口。
