# VM-K2-01：world facts provider 真机证据（2026-09-22）

设备：`192.168.3.54:5555`；游戏：monster v29 NPatch 集成包；模块 APK
`inotia4_qol_lsposed_debug_2609220236_c7e176a0843d.apk`（含 world.facts 与
cascade_stopped activate 修复 `plugin_registry.cpp`）。

## 命令（可复跑）

```bash
scripts/build-debug.sh
scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609220236_c7e176a0843d.apk
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_npatched.apk
adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common
adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1
curl -s http://192.168.3.54:8088/api/health
curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"opEnabled":true}'
curl -s http://192.168.3.54:8088/api/system/features
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"world_facts","action":"deactivate"}'
curl -s -X POST http://192.168.3.54:8088/api/ui/dialog/select -H 'Content-Type: application/json' -d '{"action":"ok"}'
curl -s -X POST http://192.168.3.54:8088/api/system/enter_slot -H 'Content-Type: application/json' -d '{"slot":0}'
curl -s http://192.168.3.54:8088/api/system/tickets/<id>
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"world_facts","action":"activate"}'
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"<consumer>","action":"activate"}'
adb -s 192.168.3.54:5555 logcat -d -v time | grep 'Inotia4Qol' > docs/history/provider-world-facts-20260922/logcat-vm-k2-01.txt
```

## 实际输出

1. 装配：`world_facts enabled`（provides `world.facts` v1.0）；autosell、extension_bag、
   inventory_hooks、quest、world_teleport 全部 `enabled` 且 `requires` 含 `world.facts`。
2. 级联停用：deactivate ticket 7 done 后 `world_facts inactive`；quest/world_teleport/autosell
   `installed_disabled` + `cascade_stopped`；extension_bag/inventory_hooks 经
   `deactivating`（在途归零）到 `installed_disabled`。
3. 恢复闭环（含 `plugin_registry.cpp` 修复）：activate world_facts → enabled；逐个 activate
   消费者后 5 个全部回到 `enabled`、资源 `live`。
4. 日志：`[world_facts] provider install/enabled: capability published/disabled: capability
   withdrawn/deactivate` 全链可见；FATAL/SIGSEGV 0；消费者恢复日志（autosell scan、EB
   move_merge、save enter）15 条。
5. 已知毛刺（登记 ledger §3）：EB/autosell 的 activate 票据 `failed`（activate 时依赖方
   尚未完成 enable，`check_requires` 即时失败），随后由 `sync_gates` 重评估自动拉起 enabled，
   票据不追认；autosell `degraded:true` 残留未清（error 已 None）。

## Host 证据

- 52/52 通过（含新增 `test_world_facts_plugin`、
  `test_cascade_stopped_consumer_explicit_activate_recovers`、
  `test_cascade_stopped_activate_missing_dependency_is_blocked`）。
- `check_arch` 全部通过、无新增违规；普通/kernel-only/probe 三种构建成功；
  kernel-only 构建的 `libgamebridge.so` 不含 `world.facts` 符号。
