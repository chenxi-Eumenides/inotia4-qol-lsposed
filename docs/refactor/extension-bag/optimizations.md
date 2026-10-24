# extension_bag 10000 袋优化清单（记录，暂不实施）

目标：扩展袋数量可配置到 10000；允许卡顿，但不能 OOM、不能 API/存档失败。
当前硬上限只有一处：事务/持久化结构体的袋号字段（已改 `int32_t`）。以下为其余
需要改造的点，按阻塞程度排序。

## 1. 存储从定长数组改为稀疏/分页

现状：`State.items[kMaxExtensionBags][16]`，`Item ≈ 268B`。
10000 袋常驻约 43MB，`StatusSnapshot` 每次发布再复制一份 → 瞬时 86MB+。

方案：
- `State` 只保留 `types/capacities`（10000 × 2B，可接受）。
- `items` 改为按袋分配：`std::vector<std::unique_ptr<BagSlots>>` 或 `std::unordered_map<int, BagSlots>`，
  只有已装备/有物品的袋才分配 16 槽。
- 空袋不分配、不序列化、不参与遍历。

## 2. 快照分层

现状：`publish_status_snapshot()` 复制全部袋与物品，status JSON 再序列化全部 payload。

方案：
- 元数据快照：`bagCount/capacities/types/mode/selected/revision`（固定小体积）。
- 物品查询：`GET /api/debug/extension_bag/bag?index=N` 按袋返回；
  status 只返回摘要，不含 payload。
- 轮询不再随袋数线性增长。

## 3. 持久化分片

现状：sidecar 单个 JSON 包含全部袋，10000 袋约 57–76MB，单次 slot_data 写入。

方案：
- 每袋一个 section/chunk（`extensionbags.bag.N`），或按 N 袋分块。
- 只写 dirty 袋；读档按需加载。
- 保留 v5 容器头（bagCount、版本、校验），旧档整体迁移一次。

## 4. 遍历与索引

现状多处 `for bag < active_bags` × `for slot < 16`：
- `module_slot_of_item_locked`
- `item_pointer_known_locked`
- ownership 审计
- 绘制与数量统计

方案：
- 维护 `item pointer → (bag, slot)` 哈希索引，O(1) 反查。
- 只遍历“已分配/非空”袋集合。
- 绘制只遍历当前选中袋（已经是）。

## 5. 代理袋与高亮

- `current_bag` 只写原版事务袋（0..4）作为代理窗口，动态取当前原版袋；
  C18 追加袋（6..17）不参与代理选择。
- 扩展袋选中态只在插件 `mode/selected` 中，不进入游戏袋号域。
- 高亮抑制仍使用“运行时原生页签数”作为临时值，绘制后恢复 raw；
  该值与扩展袋数量无关，10000 袋不受影响。

## 6. 预算参考

| 袋数 | types+caps | 全量 items（现状） | 稀疏 items（典型 5 袋使用） |
|---|---|---|---|
| 5 | 10B | 21KB | 21KB |
| 128 | 256B | 549KB | 21KB |
| 10000 | 20KB | 42.9MB | 21KB |

结论：10000 袋的关键不是字段宽度，而是“空袋不分配、快照不携带 payload、
存档分片”。完成这三项后，袋数对常驻内存和轮询成本基本无影响。
