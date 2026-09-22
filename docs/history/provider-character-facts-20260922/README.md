# VM-K2-03：character facts provider 真机证据（2026-09-22）

设备：`192.168.3.54:5555`；游戏：monster v29 NPatch 集成包（含 `character.facts` v1.0，
required 消费者 push_all/simple_mode/special_equip，仅迁移复用度 ≥2 的 `type` 与
`is_active_player_group`）。构建验证：host 54/54（含 `character_facts_plugin_tests`）、
`check_arch` 无新增违规、三种构建变体成功、kernel-only 构建不含 `character.facts`。

## 命令（可复跑）

```bash
scripts/build-debug.sh
scripts/patch-apk.sh output/<最新 debug>.apk
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_npatched.apk
adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common
adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1
curl -s http://192.168.3.54:8088/api/health
curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"opEnabled":true}'
curl -s http://192.168.3.54:8088/api/system/features
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"character_facts","action":"deactivate"}'
curl -s -X POST http://192.168.3.54:8088/api/ui/dialog/select -H 'Content-Type: application/json' -d '{"action":"ok"}'
curl -s -X POST http://192.168.3.54:8088/api/system/enter_slot -H 'Content-Type: application/json' -d '{"slot":0}'
curl -s http://192.168.3.54:8088/api/system/tickets/<id>
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"character_facts","action":"activate"}'
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"<consumer>","action":"activate"}'
adb -s 192.168.3.54:5555 logcat -d -v time | grep 'Inotia4Qol' > docs/history/provider-character-facts-20260922/logcat-vm-k2-03.txt
```

## 实际输出

1. 装配：`character_facts enabled`（provides `character.facts` v1.0）；push_all、
   simple_mode、special_equip 的 `requires` 均含 `character.facts`（special_equip 同时
   消费 `item.facts`，为首个双 provider 消费者）。
2. 级联停用：deactivate ticket 7 done 后 `character_facts inactive`；三个消费者全部
   `installed_disabled` + `cascade_stopped`。
3. 恢复闭环：activate character_facts + 逐个 activate 后 push_all/special_equip
   `enabled`；simple_mode 回到门禁关的 `installed_disabled`（`simpleModeEnabled=false`
   正确语义），`degraded=false`。
4. 日志：`[character_facts] provider install/enabled/disabled/deactivate` 全链可见；
   FATAL/SIGSEGV 0。
