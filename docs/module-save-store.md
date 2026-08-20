# 模块存档容器

## 目标与边界

每个原版存档槽 `0..2` 对应一个模块 sidecar。模块新增的持久化数据必须放入具名 section；原版 `save*.dat` 不读取、不写入、不追加且不重命名。纯内存补丁（例如堆叠上限）不使用该容器。

v1 只编排模块 API 驱动的存档生命周期：进入槽或保存成功时确保容器存在，新建槽成功时清空对应容器。功能 section 每次变更立即持久化，因此不依赖原版 UI 的保存回调。

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

section payload 对容器不透明。虚拟背包等未来功能自行定义 payload 数据结构与版本，并通过这个 API 持久化；v1 不提供 HTTP、JNI 或原版 UI 回调入口。
