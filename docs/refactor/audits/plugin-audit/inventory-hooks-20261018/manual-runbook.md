# inventory_hooks 阶段 5 人工目视 runbook（2026-10-18）

设备：`192.168.3.56:5555`，monster v29，当前模块 sha `23d8baaf…`。
存档备份：`.tmp/inventory-hooks-audit/backup/inv-hooks-save-backup.tar`（设备侧 `/sdcard/inv-hooks-save-backup.tar`）。
测试存档可丢弃；如需回滚：`adb shell su -c "cd /data/data/com.com2us.inotia4.normal.freefull.google.global.android.common && rm -rf fcea920f7412b5da7be0cf42b8c93759 && tar -xf /sdcard/inv-hooks-save-backup.tar"`。

## 卡 1：详情页出售预览与结算（monster 70%）

- 目的：验证 `UIEquip_OKDestroyItem` 接管的预演/确认两态与变体乘数。
- 前置：进档到 world；背包 bag0 有可堆叠物品（当前为「再生药水（小）」单价 4）。
- 操作：打开背包 → 点选该物品 → 点「销毁/出售」→ 观察弹窗金额 → 点「确定」。
- 预期：弹窗金额 = `unit × count × 7 / 10`（3 个 → 8）；确认后金币 +8、槽位清空；日志出现 `vanilla sell preview … multiplier=7/10` 与 `vanilla sell committed … multiplier=7/10`。
- 取证：`adb logcat --pid=$(adb shell pidof com.com2us.inotia4.normal.freefull.google.global.android.common) -d | grep "vanilla sell"`。
- 失败回滚：用上方备份恢复。
- 预计：3 分钟。

## 卡 2：任务袋透传（bag 5）

- 目的：确认模块不接管任务袋，UI 行为与原版一致。
- 前置：任务袋有物品（无则记录「无法验证」）。
- 操作：打开任务袋物品详情 → 尝试出售/销毁。
- 预期：行为与原版一致；日志**不出现** `vanilla sell`（模块未接管）。
- 取证：同卡 1 的 logcat 过滤。
- 预计：2 分钟。

## 卡 3：调试门禁关闭后的原版透传（可选）

- 目的：验证 hidden 开关关闭时 23 条 wrapper 透传。
- 前置：能通过设置页/配置接口把 `inventory_hooks.inventory_support` 置 false（kHidden，若 UI 不可见则跳过）。
- 操作：关闭开关 → 重复卡 1 的出售 → 再打开。
- 预期：关闭时出售走原版（无 `vanilla sell` 日志）；打开后恢复接管。
- 注意：`FindItem` 由 Game Host handler 替换，不受该开关控制。
- 预计：3 分钟。

## 卡 4：autosell 路径（可选）

- 目的：验证 `inventory.action.sell` 的 `source=autosell` 与 70% 报价。
- 前置：设置页开启 autosell，配置一条能命中当前背包物品的规则。
- 操作：进档等待 60 帧节拍。
- 预期：日志 `sell price source=autosell … multiplier=7/10`；金币按 70% 增加。
- 预计：4 分钟。

## 结论记录

| 卡 | 结果 | 现象/证据 |
|---|---|---|
| 1 详情页出售 | 待执行 | |
| 2 任务袋透传 | 待执行 | |
| 3 调试门禁 | 待执行/可跳过 | |
| 4 autosell | 待执行/可跳过 | |
