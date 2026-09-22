# VM-K4-04b：阶段 4 批 4b——save 领域收口（save_facts provider + save_backup 私有写 port）

2026-09-22。设备 `192.168.3.54:5555`；monster v29 NPatch 集成包
（模块 APK `inotia4_qol_lsposed_debug_2609221341_7777df5afe83.apk`）。

## 本批改动

- 新增 `contracts/game_ports/save_port.h`（`QolSavePort` v1.0，save 读 10 函数，
  含 B 类两阶段/刷新 stale 实现逐行搬运）与
  `game/adapter_ports/save_port.{h,cpp}`；`contracts/game_ports/save_call_port.h`
  （`QolSaveCallPort` v1.0，save_call 写 3 函数）+ adapter 实现。
- 新增 `plugins/save_facts/`：provides `save.facts` v1.0，requires `QOL_PORT_SAVE`，
  契约 `contracts/save_facts.h`；装配清单加入（`#if !QOL_KERNEL_ONLY`）。
- 消费者切换：`save_backup` requires `save.facts` + 声明 `QOL_PORT_SAVE_CALL`
  （读经 capability、写经 port）；`extension_bag` requires `save.facts`
  （is_ok/current_slot 改经 capability）。
- `contracts/game_host.h` + `host/game_host.cpp` 删除 save 读区与 save_call 区。

## 命令与实际输出

```bash
scripts/build-debug.sh
scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609221341_7777df5afe83.apk
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_npatched.apk
# 重启 + opEnabled + extensionBagEnabled=true 后：
curl -s http://192.168.3.54:8088/api/system/features
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"save_facts","action":"deactivate"}'
curl -s -X POST http://192.168.3.54:8088/api/ui/dialog/select -H 'Content-Type: application/json' -d '{"action":"ok"}'
curl -s -X POST http://192.168.3.54:8088/api/system/enter_slot -H 'Content-Type: application/json' -d '{"slot":0}'
curl -s http://192.168.3.54:8088/api/system/features
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"save_facts","action":"activate"}'
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"<consumer>","action":"activate"}'
adb -s 192.168.3.54:5555 logcat -d -v time | grep 'Inotia4Qol' > docs/history/gamehost-narrow-b4b-20260922/logcat-save-facts.txt
```

## 实际输出

- 装配：`save_facts enabled`（requires=[]，provider）；`save_backup enabled`
  （requires 含 `save.facts`，2 资源 live）；`extension_bag enabled`
  （requires 含 `save.facts`，29 资源 live）。
- 级联：deactivate save_facts → `inactive` + save_backup `cascade_stopped` +
  EB 经 `deactivating` 归零；activate save_facts → `enabled`；逐个恢复后
  save_backup `enabled`、EB 补一次 activate 后 `enabled`（EB 重插件恢复时序，
  首次 activate 票据未及依赖就绪）。
- FATAL/SIGSEGV 0；`git diff --check` 通过。

## Host 证据

55/55 通过（新增 save_facts 装配/fake save port 用例）；`check_arch` 无新增违规；
普通/kernel-only/probe 三种构建成功；strings 确认普通含 `save.facts`/`save_facts`
（10 处）、kernel-only 标记 0。
