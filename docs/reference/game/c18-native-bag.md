# C18 改版客户端背包

供其他插件复用。本文只描述已验证事实与安全用法；**不要在插件里硬编码地址**，
统一经 Host 适配器/能力位访问。

## 1. 版本与识别

- native bag 布局按版本号动态判定：v0.x（v0.2 / v0.38）没有 native bag；v507–v520 为 192 条旧布局（`C18Lbag{N}.dat`）；v521 及更高版本统一按 96 条新布局（`C18Lwh{N}.dat`，含 v586 / v588）。v520 的袋对象表已改为 GOT 指向的堆表（见 §3），不能只按 `0x735000` 取表。
- 能力位：`kQolTargetCapabilityC18NativeBagLayoutCandidate`（bit3，值 0x8）。
  - 已知 C18L 版本由变体表置位；未知 MD5 仅在运行期标记
    （存在 `C18BAG01` 且 `C18Lbag.dat` 存在、无 WH4）时置位。
  - 原版 / 大修 / monster / 旧 C18 为 0。
- 插件应通过 `game->target.has_capability(bit3)` 判断，不要按版本号硬编码。

## 2. 袋编号

| 袋 | 编号 |
|---|---|
| 原版事务袋 | 0–4 |
| 原版任务袋 | 5（禁止作为移动源/目标） |
| C18 追加袋 | 6–17（共 18 个袋） |

- `INVEN_GetBagSize` 的上界检查是 `cmp w0,#0x12; b.hi`，即 **18 也能通过**并索引表；
  不要把 18/19 当“无此袋”哨兵。
- C18 的 18 个袋在存档/游戏逻辑里是真实袋，不属于任何插件 sidecar。

## 3. 内存布局（仅供 game/access 层使用）

| 项 | 原版 | C18 改版 |
|---|---|---|
| 袋对象表 | `INVEN_pBagSlot`（0x7134c8） | GOT `*(base + 0x2f3bc0)` 指向的堆表（v520 为 18 项） |
| 布局标记 | — | `base + 0x735000` 只保留原版 6 个袋的镜像 |
| 袋槽表 | `INVEN_pItem`（0x7131c0） | `base + 0x735100` |
| 每袋槽跨度 | — | `0x80` |
| 上界 | 6 袋 | 18 个真实袋（索引 0..17）；索引 18 不是真实袋 |

探测规则（`game/access/inventory.cpp`）：
- `base+0x735000` 首指针非空 → C18 布局；否则原版。
- 真实袋数从 GOT 袋表统计，不能用 `0x735000` 的镜像表。
- 每个非空表项按袋对象校验：可读、类别 1..4、`+0x10` 容量位域 ∈ {4,8,12,16}；取最高有效索引 + 1，夹在 6..32。
- **禁止**用 `INVEN_GetBagSize(index)` 做逐项校验：索引 18 会解引用非袋指针并崩溃（v520 tombstone 于 `INVEN_GetBagSize+24`）。
- 袋对象表在进世界后可用，**不需要打开背包**；不要在 UI 未打开时依赖控件树。

## 4. 袋数量查询（UI 无关）

经 Host 袋数探测获取（GOT 表 + 袋对象校验）；不要在插件里遍历 `bag_size(0..18)`，索引 18 会崩溃。
该结果与原生页签数量一致，但不需要背包面板。

不要用控件子节点数量作为“袋数”：那只在面板打开时有效，且会混入插件页签。

## 5. UI 布局（仅 UI 层）

- 18 个 `user_type == 2` 的页签控件。
- v513：三列，x=0（6×57×57）、x=54 / 108（各 6×40×40），行 y≈0,70,140,210,280,350。
- v514：第一列 6×57×57；右侧竖列 x=64、y=0..350（6×40×40）；
  底部横排 y=420、x≈-326..64（6×40×40）。
- 触摸诊断：点击 child 6/11/12/17 后 raw `current_bag` 变为同号值；
  模块页签退出逻辑不能只扫 child 0..5。
- 页签数量/rect 只属于 UI 适配层，其他插件不要依赖。

## 6. `current_bag` 语义

- 游戏全局“当前袋”，原版 `DrawInvenBag` / `RefreshItemArea` / `INVEN_GetBagSize`
  都直接读它；没有“显示某个袋”的参数。
- 写入上界：C18 为原生袋上界（≤18），原版 6；插件逻辑袋号**不得**写入。
- 高亮抑制的临时值用“原生袋数/页签数”（动态），绘制后必须恢复 raw 原值。
- 代理袋：插件投影只借用原版事务袋 0..4 作为窗口；扩展袋内容通过该窗口显示，
  扩展袋号只存在插件内部状态。

## 7. 物品访问

- 安全接口：`item_facts.item_ref_at(bag, slot)` 返回 `slot + generation`；
  移动/删除前必须复核 generation。
- `detail_item_context` 在 C18 上可能返回 `desc=0 / bag=-1`（VMA 偏移失效），
  **不可依赖**；来源解析用 moving 控件的物品指针 + raw 当前袋按物理槽反查。
- 删除/插入用原生 `remove_item_direct` / `SaveItemOnEmpty`，并在操作后复核目标/源。

## 8. 其他插件接入建议

1. 只通过 Host 契约访问：`inventory_extras.read.bag_size`、`item_facts.*`、
   `read_only.item.*`、`target.has_capability`。
2. 需要遍历 C18 袋时：`bag < native_bag_count` 且 `bag != 5`。
3. 不要假设 6 是“扩展袋起点”或哨兵；C18 下 6 是真实原生袋。
4. 不要缓存袋对象/物品裸指针跨帧；用 handle（slot+generation）。
5. 需要 UI 页签/rect 时，必须在背包面板打开且通过 UI 适配层获取。

## 9. 已知限制

- 袋对象表在进入世界前可能为空；探测应在进档后做一次，换档失效重探。
- 18 是 getter 可接受索引，但不是真实袋；不要持久化为“无选中”。
- C18 追加袋与插件扩展袋是两个独立编号空间，禁止混用。

## 10. Native bag 存档文件与备份策略

### 10.1 已验证布局

- 静态证据来自 fresh arm64 `libgame.so`：`apk/decoded/c18-v514/lib/arm64-v8a/libgame.so`、`apk/decoded/c18-v520/lib/arm64-v8a/libgame.so`、`apk/decoded/c18/lib/arm64-v8a/libgame.so`。
- v514/v520 旧布局每槽文件为 `C18Lbag{N}.dat`，loader 固定读取 192 条记录（袋 6..17）；v586 新布局每槽文件为 `C18Lwh{N}.dat`，loader 固定读取 96 条记录（袋 6..11）。
- 两版都使用 `C18BAG01` magic、20-byte header 和相同 `0x230` record schema。小端 header 字段为 `+8=1`、`+0x0c=count`、`+0x13=slot+1`；v514/v520 count=`0xc0`，v586 count=`0x60`。精确文件长度分别是 `20 + 192*0x230` 与 `20 + 96*0x230`。
- header `+0x10..+0x12` 三个保留字节语义未证；备份按 opaque bytes 保存并在目标文件中原样复制。
- loader 的记录循环次数是版本代码中的固定常量，**不会用 `+0x0c` 决定读取条数**。因此只改 count 而不写实际 192/96 条记录不是合法转换。
- 记录体按 opaque bytes 处理。loader 判断 empty record 时检查首五个 8-byte 值，全零即空记录；其他字节不由备份层解释或重编码。

### 10.2 文件读写边界

- 文件以 stdio 直写，未发现原版 `SAVE_Encrypt` 或设备密钥加密证据；C18 native bag image 不做加解密。
- `SAVE_Save` 和 C18 helper 会写入真实当前槽，不是纯序列化 API；导出必须直接读取活动文件，不能调用它们。
- 文件都位于 Native init 传入的动态 `data_dir` 根目录；slot 0/1/2 的文件后缀 N 分别为 1/2/3：旧版 `C18Lbag{N}.dat`、新版 `C18Lwh{N}.dat`。不得硬编码包路径。
- primary 缺失时只读同一 `data_dir` 根目录的 shared fallback `C18Lbag.dat`；fallback header 可为 96 或 192 条。primary 和 shared 都不存在时导出 fail-closed（无 image 可收集）；导入旧备份（v1-v3，bundle 不含 image）时按目标布局生成空 image（192/96 条全零记录，扩展背包为空）后继续。不得调用内部 selector `0xc5538`，因为它会通过 `system()` 复制文件。
- shared fallback 导入后只 materialize 为目标版本活动 per-slot 文件，不改写 shared 文件。

### 10.3 跨版本备份策略

- bundle 只保存导出时当前游戏实际生效的一份 C18 image，不收集另一文件族。v520/v586 文件名不同不代表记录 schema 不同。
- 旧目标实际写 192 条；新目标实际写 96 条。96→192 在尾部补 96 条全零记录；192→96 保留前 96 条并直接丢弃尾部。该截断是用户确认的导入语义，目标槽中超出前 96 条的数据会被覆盖丢失；原 bundle 保留完整源记录。
- 历史 v514/v520→v586 未发现游戏自身的 `bagN`→`whN` 迁移事实；备份导入转换不应被描述成游戏迁移。
- 静态逆向证据不等于设备验收：本文件所述版本间导入/loader 行为尚未做设备 A/B，不能据此宣称真机通过。
