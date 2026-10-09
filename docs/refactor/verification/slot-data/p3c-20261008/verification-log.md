# P3c 真机验证记录

## 结论与范围

**部分通过；P3c 整体仍为 `NOT_ACCEPTED`。** original v1.3.2 与 monster v31 已验证保存正常路径、EB 数据捕获/恢复、部分 slot 生命周期和原版物品 API 操作。未完成真机 prepared/confirmed/final 三个崩溃恢复窗口、VM-SD-04 的 P5 导入/回滚、overhaul（按用户决定不测）、VM-B 卡物理触摸交互、旧 epoch 句柄设备级拒绝，以及设备上的 hook/trampoline 结构解析日志。Host/静态/单卡 PASS 不代表整张 P3c 验收完成。

相关提交：P3c 切换 `730ff84d`；修复批 `f9948631`、`f4079718`、`a0836757`、`0ca4e862`、`f06900fa`、`b82f1514`、`633d8ce1`、`d2870180`、`83f1459d`；路径修复 `48fc9d44`；面板清空修复 `26b19fb8`。

## 部署与数据保护

- 设备 `192.168.3.54:5555`；包 `com.com2us.inotia4.normal.freefull.google.global.android.common`。使用 LSPatch 集成包，`install -r` 后 force-stop+monkey、health ready，并以 logcat `module identity ... sha256=` 核对实际加载模块。
- 早期 P3c 构建 SHA `77d0e57569ef…` 用于 original 与首次 monster 功能卡；修复后的 monster 模块 SHA `121d4e97400c0c14391448536aa226ad8b2d259600263d229df3f5f496e85f94` 用于面板数据保留及重跑 VM-B01..04。两者按卡区分，不把早期构建测试冒充修复后全量回归。
- 游戏存档、sidecar 与配置备份留在 `.tmp/p3c-device-20261008/`，含真实用户测试数据，**不提交**。本文件仅保存脱敏后的状态、票据与哈希前缀；不含 dat/sidecar 原始内容、配置全文、全量 logcat 或 APK。

## §1 本批发现与修复

### SAV-P3C-01：游戏返回的 evidence 文件名含目录段

首次原版保存时 `SAVE_GetSaveFileName` 返回应用目录内的相对路径（含目录段），而 Adapter 只接受纯文件名，造成 dat 已写、sidecar 不提交并进入 degraded。失败路径保留 journal，未提交错误配对数据。`48fc9d44` 修复为校验应用目录前缀并解析相对/绝对路径，拒绝越界、`.`、`..`、空段、`//` 和超长输入。

### EB 面板打开清空已加载状态

早期构建打开 inventory panel 后，旧 draw-end 清理将 PopupState=4 当作主菜单，执行整对象清零；P3c 移除惰性加载后没有重新读回。捕获日志从 `bytes=3584 nonempty=1` 降为 `bytes=2147 nonempty=0`。`26b19fb8` 改为仅依据可靠 world state 清理真正主菜单/离开世界状态，并在未准入时保留 state。修复后打开/关闭面板 types、capacities、33 格物品和 capture 大小保持不变。

## §2 真机子项

| 子项 | 变体/构建 | 结果与边界 |
|---|---|---|
| VM-SD-03 正常保存配对 | original v1.3.2，`77d0e575…`；monster fixed build 复核正常保存 | save ticket `done`，成功体逐字 `{"ok":true,"slot":0,"sidecar":"ensured"}`；dat、sidecar 同一保存均变化，journal 清除、last-good 正常轮转。启用 EB 后 capture `3584B/nonempty=1` 并完成联合提交。**不覆盖 crash/restart 三窗口。** |
| VM-SD-01 多槽/同槽重入 | original v1.3.2，`77d0e575…` | `main_menu→enter_slot 1→0→0` 均 done；slot1/slot0 EB state hash 不同且回到 slot0 后精确复原；epoch 2→3→4、`parsed=1`；加载前有 session-not-admitted；转换期 sidecar hash 不变。旧 epoch 句柄设备级拒绝无直接 API 证据。 |
| VM-SD-02 当前档启停补加载 | original v1.3.2，`77d0e575…` | `opEnabled=true` 后 EB deactivate/activate 均 done；停用时 EB owner 503，启用后 state hash 恢复为 `49c24ac5…`，load 日志两次 `parsed=1`；测试后 opEnabled 已关闭。插件启停不递增 slot-session epoch。 |
| VM-SD-04 建档默认态 | original v1.3.2，`77d0e575…` | slot1 旧 EB section reset 后，自动 skip 剧情 API 驱动 create ticket 最终 done，结果 `{"ok":true,"slot":1,"created":true}`；新 slot 记录 `presence=missing` 并加载默认空态，不继承旧 EB 数据。此前未及时 skip 时 ticket timeout 是测试调度顺序导致；本次 API 驱动剧情后创建成功。导入/回滚仍留 P5。 |
| VM-B01 消耗品 | monster v31，fixed SHA `121d4e97400c…` | `use_item bag0 slot0` done，药水 2→1；API 操作通过，不代表物理点击基线。 |
| VM-B02 装备 | monster v31，fixed SHA `121d4e97400c…` | role=`main` 400；role=0 成功，鞋装备到 equip_slot4，真机复跑成功。 |
| VM-B04 原版袋内移动 | monster v31，fixed SHA `121d4e97400c…` | bag0 slot7→slot6 done，保存后回主菜单重进仍在 slot6；API 移动和持久化通过，未测触屏拖动。 |
| VM-B03 宝石镶嵌 | monster v31，fixed SHA `121d4e97400c…` | `op/inventory/add` 构造带孔鞋与宝石；equip、put_jewel 均 done，装备 slot4 出现力量宝石 value=1024，背包宝石消耗；保存 ticket57 done，回主菜单重进后宝石仍在。测试物品系用户许可的变更。 |
| Monster SAVE_Save 行为侧证据 | monster v31，`77d0e575…` / fixed `121d4e97400c…` | health `series=monster, version=v31`、模块 SHA 匹配；多次保存 ticket done，dat/evidence 路径、sidecar capture 正常，无新增 WARN/ERROR/crash/degraded。静态复审确认调用交还 trampoline 且未从 `+4` 绕过；设备无 hook entry/B-target/backup 结构日志，不能声称直接观测到 guard 地址。 |
| EB 面板与 EB payload round-trip | monster v31，fixed SHA `121d4e97400c…` | `open_panel` 后 types/capacities/33格稳定、capture 3584/nonempty=1；EB API 将 category241 写入 index1 slot3，save done，sidecar hash `e089c6b110a4→f6db71660ed7`；重进后 item 保留。`exit_view` 单独返回 `exit extension view failed`，随后 `close_panel` done 回 original mode，数据仍正确保存/恢复。 |

## §3 未覆盖项与证据限制

1. VM-SD-03 prepared/confirmed/final crash/restart 窗口未真机注入；Host 故障注入不替代设备日志，恢复卡仍 `NOT_ACCEPTED`。
2. VM-SD-04 导入、完整回滚、primary 已应用而 last-good 失败留 P5。
3. VM-SD-01 旧句柄直接请求拒绝仅有 Host/provider 门禁证据，无生产 API 能安全构造旧句柄请求。
4. VM-B01/02/04 为 API 业务操作，不是触摸 click/drag；VM-B03 在 fixed build API 路径通过。overhaul 不做真机按用户裁决。
5. Monster B 跳板 guard 有真实保存成功行为侧证据和静态 trampoline 复审，但设备无解析地址/backup 结构性日志；作为观测欠账保留。
6. `exit_view` 单独失败未登记为产品缺陷：随后 `close_panel` 成功恢复 original mode，数据保存并重进一致；本记录保留调用边界。

## 证据保留

可重跑的请求脚本、部署缓存与真实测试前保护备份位于本地 `.tmp/p3c-device-20261008/`，不得 stage。此日志只保留脱敏结果与短哈希；不得把个人存档二进制、配置全文、完整设备 trace 或 APK复制进长期证据目录。
