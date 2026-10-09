# P4 autosell slot_data 验证记录

## 结论

**数据路径真机子项通过；P4/VM-44 整体仍为 `NOT_ACCEPTED`。** monster v31 验证了 per-slot Immediate 写、进程重启恢复、槽隔离、direct API 与 autosell 扫描卖出 EB 物品并保存/重进。VM-17/18 的装备页/商店 UI 操作尚未完成，且商店页发生 crash，用户要求等扩展背包专项审阅后整体检查修复；真实 Provider 并发/注销 drain 也未由设备证据覆盖。P5 暂不开始。

代码提交：`63912a4e`（首版 slot_data 接入）、`07779c4d`（在途 revision/失败重试/生命周期修复）、`6db1c866`（unregister 在途失败回归）、`3f018c7c`（load未就绪及同步回调边界修复）。

## 构建与 Host 验证

- Debug APK：`output/inotia4_qol_lsposed_debug_2610081804_e4578153818c.apk`，APK SHA-256 `e4578153818c1889289a0fbf63d51712c08d26693f0ed1dd77f1cc0052710a54`。
- LSPatch monster 集成包：`output/Inotia4_v1.3.2_monster_v31_lspatched.apk`，SHA-256 `3b576768c5ef20a61226f9e85adde08f3fb278f61a212e62d6855027ed50d60b`；embedded module SHA 与普通 Debug APK 一致。V2 signer SHA-256 `a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc`，install-r 后 `firstInstallTime=2025-06-13` 保持。
- Host 全量 CTest `120/120`；`autosell_slot_data_tests` 最终 `192/192`；`check_arch.py` 新增违规 0；`check_http_routes.py` 146；`scripts/build-debug.sh` 成功。
- 直接 section store IO 检查：`plugins/autosell/**` 与 `bridge/native/gamebridge_autosell.cpp` 均无 `store.read/write` 或 `QolStoreApi read/write` 命中。配置读写由 slot_data participant 承担；save-enter event 保留扫描生命周期用途，不再驱动 section 读取或决定写入槽。

## VM-44 真机子项

设备 `192.168.3.54:5555`，monster v31；实际加载 SHA 与上述 build 一致。

| 子项 | 操作与结果 | 判定 |
|---|---|---|
| Immediate per-slot 写 | 当前 slot0 修改规则为 `enabled=false, rarity=4, special=[dice]`，不调用 `/api/system/save`；dat MD5 `75f24e2206cf…` 不变，slot0 sidecar 从 `2382816e1da7…` 改变；slot1 sidecar `21408489bf99…` 不变。 | 通过（限定 slot0 immediate 写） |
| 进程重启恢复 | force-stop+monkey、关闭 agreement、重新进入 slot0 后自定义规则恢复。 | 通过 |
| 多槽隔离 | enter slot2 后读默认 `rarity=0,special=[]`，slot2 sidecar MD5 `3debcc90358e…` 不变；回 slot0 自定义规则仍恢复。 | 通过 |
| 扫描总开关 | `autoSellEnabled=false` 时库存不变；启用全局开关后 scan 启动并产生销售统计；关闭后日志 `task removed`。 | 通过（扫描启动/停止） |
| direct API EB sale | `sell_item bag6 slot2` ticket `done`、price=2229，EB 槽清空；save done、重进 slot0 后仍空。bag7 slot0/category241 同样 ticket `done`、price=11，保存/重进保持空；原版 bag0 slot6 对照 ticket `done`、price=14。 | 通过（API/neutral provider路径） |
| autosell EB scan | 诊断日志首次显示 bag6 slot9/category2 `view=ok`、`decision=match`、但内部袋索引与逻辑袋号比较导致 `sell_status=stale`；修正映射后，同构建 special=`backpack` scan 输出 bag6 slot9 `sell_status=ok`，统计 `sold +1, failed +0`；save/reload 后 EB slot6/9为空。 | 通过（monster diagnostic build限定路径） |
| 清理测试规则 | disabled rules 写入前 slot0 sidecar `94d34ce4f88b…`，实际写后 `ec682e14cfb9…`；等 hash 改变后再重启，slot0 读回 `enabled=false,rarity=0,special=[]`；`autoSellEnabled=false`、`opEnabled=false`。 | 通过 |

## 未覆盖与限制

1. ITM-16 初始 direct API 与 autosell EB sale失败，后经 EB 3.1 sale callback、neutral provider direct API callback 和 internal/logical bag index映射修复。当前 API/autosell 成功子路径有真机证据；装备页与商店 popup 的独立 UI sale callback尚未复验，不能由这些 API 子项代替。
2. Host 专项以 fake `QolSlotDataApi` 驱动 autosell callbacks，覆盖 revision、pending、retry、unregister与槽隔离逻辑；不等价于真实 core.save provider 的并发队列/注销 drain 集成证明。
3. Oracle 静态复审确认直接 store IO、section v2、两策略、错误 presence 不直接覆盖运行态、错误 load 关闭扫描等主要规则；同时指出 `autosell_store_persist(-1, config)` 仍是潜在独立pending入口（全仓搜索只有声明/定义，当前无生产调用点）、request callback同步重入仅fake覆盖、非durable dirty在unregister后依赖后续同槽load catch-up。以上限制不得被写成“durable unregister drain”或“真实 Provider 并发已验证”。
4. 无session pending的有效已有section优先级由代码路径支持，但缺少专用直接测试；设备未测 unload期间进程被杀、错误 presence、Provider并发/同步callback压力。

## VM-17/18 手动 UI 卡的中止记录

2026-10-09，用户尝试无法进入扩展袋，并在打开商店页时游戏崩溃；用户指示所有涉及扩展背包的测试延至扩展背包专项审阅后统一检查修复。只读 crash buffer/tombstone 已保留在本地 `.tmp/it16-device-20261009/shop-crash-logcat.txt`、`shop-tombstone.txt`；模块 identity 为 `5dc50f2a2cc6…`。符号化栈：`LINKEDLISTITEM_getData` → `UIEquip_DrawInvenBag+68` → module `write_extension_draw_original_bag()` (`game/adapter_ports/extension_bag_write_port.cpp:230-237`) → `store_draw_inven_bag_group_wrapper()` (`plugins/extension_bag/extension_bag_store.inc:619-637`) → `Scene_Draw_POPUP_SC_STORE+124`。崩溃发生于 EB 商店原版袋绘制 wrapper 调用，不在 autosell/direct API sale callback；根因需在扩展背包专项审阅中确认。本轮未重启设备、未继续测试 EB UI；VM-17/18 维持 `NOT_ACCEPTED`/待专项审阅。

## 设备数据保护

测试前归档 `.tmp/p4-device-20261008/p4-pre.tar`（SHA-256 `d0ad791872450afddcee4f874e1c745e947ca416cff49af6b008d765ccf7759a`）及 `previous-output/` 的旧集成 APK。测试过程未保存原版 dat；售出变更未调用游戏保存，重启后按原 dat 恢复。所有真实存档、sidecar、配置与完整设备日志留在本地 `.tmp/`，不提交。
