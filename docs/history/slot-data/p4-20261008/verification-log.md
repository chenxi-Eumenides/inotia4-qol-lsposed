# P4 autosell slot_data 验证记录

## 结论

**部分真机通过；P4/VM-44 整体仍为 `NOT_ACCEPTED`。** 当前实现已通过 Host、架构、路由和 Debug 构建验证；monster v31 真机验证了 per-slot Immediate 写、进程重启恢复、slot 隔离及实际 autosell scan 启停。实际扫描观察到 EB bag6/7 的 `RemoveFailed`，且 provider 并发/注销 drain 尚非真实 adapter 集成测试；不能把 P4 标为已接受，也不据此启动 P5。

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
| 实际出售 | rules `enabled=true,rarity=1` 后累计 `sold=3,failed=16`；原始物品未调用 `/api/system/save`，force-stop重启后库存由原版 dat 恢复。失败日志来自扩展袋 bags 6/7 的 `status=10/kRemoveFailed`，不是本批 slot_data 读写路径。 | 部分通过；VM-44 整体仍 NOT_ACCEPTED |
| 清理测试规则 | disabled rules 写入前 slot0 sidecar `94d34ce4f88b…`，实际写后 `ec682e14cfb9…`；等 hash 改变后再重启，slot0 读回 `enabled=false,rarity=0,special=[]`；`autoSellEnabled=false`、`opEnabled=false`。 | 通过 |

## 未覆盖与限制

1. `RemoveFailed` 静态调用链为 `autosell/scan.cpp` → `inventory.action.sell` → `inventory_hooks` 的 `INVEN_RemoveItem`；失败会退款。测试没有证明该 EB 移除限制的完整底层原因，也没有显示它由 P4 persistence 改造引入。需单独调查 EB sale 操作路径；本卡不能因 persistence 子项通过而整体 PASS。
2. Host 专项以 fake `QolSlotDataApi` 驱动 autosell callbacks，覆盖 revision、pending、retry、unregister与槽隔离逻辑；不等价于真实 core.save provider 的并发队列/注销 drain 集成证明。
3. Oracle 静态复审确认直接 store IO、section v2、两策略、错误 presence 不直接覆盖运行态、错误 load 关闭扫描等主要规则；同时指出 `autosell_store_persist(-1, config)` 仍是潜在独立pending入口、request callback同步重入仅fake覆盖、非durable dirty在unregister后依赖后续同槽load catch-up。以上限制不得被写成“durable unregister drain”或“真实 Provider 并发已验证”。
4. 无session pending的有效已有section优先级由代码路径支持，但缺少专用直接测试；设备未测 unload期间进程被杀、错误 presence、Provider并发/同步callback压力。

## 设备数据保护

测试前归档 `.tmp/p4-device-20261008/p4-pre.tar`（SHA-256 `d0ad791872450afddcee4f874e1c745e947ca416cff49af6b008d765ccf7759a`）及 `previous-output/` 的旧集成 APK。测试过程未保存原版 dat；售出变更未调用游戏保存，重启后按原 dat 恢复。所有真实存档、sidecar、配置与完整设备日志留在本地 `.tmp/`，不提交。
