# VM-K2-02：item/inventory facts provider 真机证据（2026-09-22）

设备：`192.168.3.54:5555`；游戏：monster v29 NPatch 集成包（含 `item.facts` v1.0 与
7 个 required 消费者切换）。构建验证：host 52+ 通过（含 `item_facts_plugin_tests`）、
`check_arch` 无新增违规、三种构建变体成功、kernel-only 构建不含 `item.facts` 符号。

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
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"item_facts","action":"deactivate"}'
curl -s -X POST http://192.168.3.54:8088/api/ui/dialog/select -H 'Content-Type: application/json' -d '{"action":"ok"}'
curl -s -X POST http://192.168.3.54:8088/api/system/enter_slot -H 'Content-Type: application/json' -d '{"slot":0}'
curl -s http://192.168.3.54:8088/api/system/tickets/<id>
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"item_facts","action":"activate"}'
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"<consumer>","action":"activate"}'
adb -s 192.168.3.54:5555 logcat -d -v time | grep 'Inotia4Qol' > docs/history/provider-item-facts-20260922/logcat-vm-k2-02.txt
```

## 实际输出

1. 装配：`item_facts enabled`（provides `item.facts` v1.0）；attribute_quality、autosell、
   craft_ui、custom_recipe、extension_bag、inventory_hooks、special_equip 的 `requires`
   均含 `item.facts`。
2. 级联停用：deactivate ticket 7 done 后 `item_facts inactive`；attribute_quality/autosell/
   custom_recipe/special_equip `installed_disabled` + `cascade_stopped`；
   extension_bag/inventory_hooks/craft_ui 经 `deactivating` 在途归零到停用态。
3. 恢复闭环：activate item_facts + 逐个 activate 消费者后 **8 插件全部 `enabled`**；
   extension_bag/inventory_hooks/special_equip/attribute_quality/item_facts `degraded=false`。
4. 日志：`[item_facts] provider install/enabled: capability published/disabled: capability
   withdrawn/deactivate` 全链可见；FATAL/SIGSEGV 0。
5. 已知毛刺（沿用 ledger §3 已登记条目）：autosell/craft_ui/custom_recipe 恢复后
   `degraded:true`（optional 依赖恢复时序），终态功能 enabled。
