# 扩展背包投影：读取入口虚拟化（不写 `INVEN_pItem`）

状态：已实现（装备页、商店、合成投影均使用读取入口虚拟化；容量通过 `INVEN_GetBagSize` hook 提供）。
范围：`plugins/extension_bag` 的装备页/商店/合成物品网格显示。
目标：扩展物品在面板期间由原版控件显示，但 `INVEN_pItem[bag][slot]` 始终保持原版真实值；
`core.item` 读原版空间不再看到扩展物品。投影期间不写 `INVEN_pItem` 或袋容量字。

## 1. 问题

现有两条显示路径：

1. **控件级后绑定**：先让原版 `UIEquip_RefreshItemArea` 按槽表刷新控件，再把控件 item 改成扩展物品。
   第一帧原版物品已经画出来，随后才被替换，产生闪烁。
2. **槽表覆盖**：把扩展物品指针临时写进 `INVEN_pItem[bag][slot]`，刷新后控件自然显示扩展物品。
   不闪烁，但 `core.item` 读原版空间时会把扩展物品当成原生物品，出现重复/误移动。

用户约束：`INVEN_pItem[bag][slot]` 不允许改动。

## 2. 原版读取路径（反汇编证据）

`UIEquip_RefreshItemArea @ 0xb7a00`（arm64）：

```
b7a1c  ldr  x0, [GOT 0x2f5000+0x6d8]   ; UIEquip 当前袋
b7a28  ldr  w0, [x0]
b7a2c  bl   INVEN_GetBagSize            ; 容量
...
b7a70  ControlObject_GetChild(root, slot)
b7a84  cmp  capacity, slot              ; 槽 < 容量 → 显示
b7a38  ControlObject_SetActive(ctrl, 1)
b7a44  ControlObject_SetShow(ctrl, 1)
b7a48  ldr  x1, [x23, #0x6d8]           ; 当前袋
b7a50  ldr  x2, [x24, #0x1c8]           ; INVEN_pItem 数组
b7a58  add  x1, slot, current_bag<<4
b7a60  ldr  x1, [x2, x1, lsl #3]        ; 槽内物品指针（唯一数据源）
b7a64  bl   ControlItem_SetItem(ctrl, item)
...
槽 >= 容量 → SetActive(0)/SetShow(0)
```

要点：

- 槽表只在 `b7a60` 读一次，随后立即交给 `ControlItem_SetItem`。
- 控件数据是原版唯一的显示副本；绘制/点击/详情都读控件 item。
- 因此**在 `ControlItem_SetItem` 入口替换 item**，就能让第一帧控件直接持有扩展物品，
  不需要先画原版再改，也不触碰槽表。
- 容量通过 `INVEN_GetBagSize` 读取；当前 hook 在投影窗口返回扩展袋容量，普通路径透传原函数。

## 3. 设计

### 3.1 Hook `ControlItem_SetItem`

```
void ControlItem_SetItem(void* control, void* item)
```

Wrapper：

```
if (投影显示上下文匹配 control 且 slot < 容量)
    item = 扩展物品对象（空槽为 nullptr）
call original(control, item)
```

- `control` 通过 `valid_child_locked(projected_item_root, slot)` 匹配；
- 扩展袋 = `module_view_index`（内部 0 基），槽 = 匹配到的 child index；
- 物品 = `module_item_locked(bag, slot)`（空槽返回 nullptr）；
- 不写 `INVEN_pItem`、不改容量字。
- 第一帧即扩展物品，无闪烁。

### 3.2 安装期 pending 上下文

`install_module_view_locked` 目前在 refresh 之后才设置 `module_view_installed`，
所以 refresh 期间 hook 看不到投影状态。增加“待提交上下文”：

```
begin_pending(bag, window_bag)
original refresh
... 校验 root/controls/ownership ...
commit()   // 转为 committed 状态
```

`begin_pending` 只记录 `{bag, window_bag}`；hook 每次用 live
`extension_bag_ui_root(kEquipItemRoot)` 取控件树，避免 root 在 refresh 中重建导致旧指针。
安装失败时 `cancel()`，随后 restore refresh 必须走 suppression。

### 3.3 恢复期 suppression

`restore_module_view_locked` 会在 `module_view_installed` 仍为 true 时调用原版 refresh。
若不抑制，hook 会把扩展物品又写回控件。增加 RAII：

```
DisplaySubstitutionSuppressScope suppress;
original refresh;
```

`fail_install` 的恢复 refresh 同样加 suppression。

### 3.4 `ControlItem_GetItem` 安全网

```
void* ControlItem_GetItem(void* control)
```

Wrapper：若投影上下文匹配，返回扩展物品；否则调用 original。

- 覆盖 SetItem 被绕过、或后续读取控件数据的路径；
- `SetItem` 已替换时返回值一致；
- 不改变控件数据，只做读侧保证。

### 3.5 交互

控件 item 是扩展对象，后续 UI 适配识别扩展袋来源并把物品操作交给 `core.item`：

- 点击、详情、使用、装备、出售由 UI 适配与 `core.item` 操作面处理；
- `core.item` 读 `INVEN_pItem` 看到真实原版物品，不会重复。

### 3.6 资源声明

声明式 entry hooks：

| 资源 id | 符号 | 用途 |
|---|---|---|
| `hook:ControlItem_SetItem` | `F_CONTROL_ITEM_SET_ITEM_VMA` | 投影期间替换控件 item |
| `hook:ControlItem_GetItem` | `F_CONTROL_ITEM_GET_ITEM_VMA` | 读侧安全网 |
| `hook:INVEN_GetBagSize` | `F_GET_BAG_SIZE_VMA` | 投影窗口返回扩展袋容量，不写容量字 |

backup 写入 `ExtensionBagUiState`：

```
void* backup_control_item_set_item;
void* backup_control_item_get_item;
void* backup_get_bag_size;
```

`extension_bag_bind_backup` 按资源 id 分发。`ControlItem_SetItem` 是投影安装的必需 hook；
backup 缺失时 `install_module_view_locked` fail-closed，不再退回后绑定闪烁路径。

### 3.7 重入与回滚

- `module_item_locked` 可能释放 stale 对象并清空控件，进而再次调用 `ControlItem_SetItem`。
  用 thread-local 替换深度守卫：深度 > 0 时一律走 original，不再替换，避免递归。
- `fail_install` 在清空控件和恢复 refresh 前先 `cancel + suppression`；
  否则清空控件的 `SetItem(nullptr)` 会被 hook 再写回扩展物品。
- `restore_module_view_locked` 在 refresh 前 `cancel + suppression`，保证控件一次性读回真实槽表。

### 3.8 容量读取虚拟化（已实现）

`UIEquip_RefreshItemArea` 通过 `INVEN_GetBagSize(current_bag)` 取容量。
`hook:INVEN_GetBagSize` 在投影窗口返回由扩展袋 catalog 派生的容量，其他调用透传原函数；
投影安装/恢复不写 `set_bag_size_word_locked` 所操作的容量字。

- `core.item` 的 `native_bag_capacity` 直接读袋对象容量字，不经过该函数，因此不受影响；
- 其他 UI 读 `INVEN_GetBagSize` 时看到扩展容量，符合显示语义；
- 存档/FindSaveSlot 直接读袋对象，仍看到真实容量。

## 4. 不变量

1. 投影显示不得写 `INVEN_pItem[bag][slot]`。
2. 控件 item 是显示副本；`core.item` 不读控件。
3. 每个投影安装/恢复必须成对调用 pending/cancel/commit 与 suppression。
4. root/control 指针只在当前帧有效；每次替换用 live root 重新匹配。
5. 安装失败必须抑制替换并恢复控件，不能留下扩展控件数据；原始容量字保持不变。
6. `ControlItem_GetItem` 只在匹配当前投影根时替换，其他控件一律 original。

## 5. 验收

- 装备页进入扩展袋：第一帧即扩展物品，无原版物品闪烁。
- 退出扩展袋：第一帧恢复原版物品，无扩展物品残留。
- 投影期间 `INVEN_pItem` 字节不变；`core.item` 读原版空间只看到原生物品。
- 移动/出售/使用/装备扩展物品由 `core.item` 操作面按扩展袋来源处理。
- hook 安装失败时投影 fail-closed，不出现半投影。
- 装备页、商店与合成均验证首帧物品和容量显示；`INVEN_pItem` 与容量字保持原值。
- 真机：v513/v514/v520 打开/切袋/拖动/退出/存档。
