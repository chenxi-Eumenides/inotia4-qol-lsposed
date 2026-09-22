# VM-K4-02：阶段 4 批 2——world/item/character 三 provider 脱离 QolGameHost

2026-09-22。设备 `192.168.3.54:5555`；monster v29 NPatch 集成包
（模块 APK `inotia4_qol_lsposed_debug_2609221143_4a2a33e64cbe.apk`，含 typed handler
幂等重注册修复）。

## 本批改动

- 新增 `contracts/game_ports/{world,item,character}_port.h` 与
  `game/adapter_ports/{world,item,character}_port.{h,cpp}`：三 provider 的低层事实来源
  从 QolGameHost 迁至受限 Adapter Port（固定类型函数表，版本 v1.0）。
- 三个 provider descriptor 声明所需 port；install 经 `ctx.ports` 注入（fail-closed）；
  **`ctx.game` 引用清零**（grep 验证 0 残留）。
- `contracts/game_host.h` + `host/game_host.cpp` 删除仅 provider 消费的函数：
  world 读 8 项、item 6 项、inventory 4 项、character 2 项（grep 确认无其他调用点）；
  保留仍被直连消费者使用的函数（character 扩展读、inventory 容量/游标类、
  item ability_level、item_count facade、craft/save/quest/choice、全部写面）。

## 过程中发现并修复的引擎缺陷

typed command handler 重复注册非幂等：级联停用路径只调 `on_disabled`（不注销
handler），重 install 的 `register_typed_handler` 对同 id 冲突 → owner install 失败。
真机表现为 world_facts 级联后 quest activate 报 `failed "plugin install failed"`。
修复：`infra/commands.cpp` 同 id 同 handler 幂等重注册（更新 ctx），同 id 异 handler
仍 conflict；host 用例覆盖两条路径。修复前 EB/autosell 的 activate 票据 `failed` 后由
`sync_gates` 拉起不追认票据的毛刺在本轮复验中也未再出现。

## 命令与实际输出

```bash
scripts/build-debug.sh
scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609221143_4a2a33e64cbe.apk
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_npatched.apk
# 重启 + opEnabled 后依次对 world_facts/item_facts/character_facts 执行：
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"<provider>","action":"deactivate"}'
curl -s -X POST http://192.168.3.54:8088/api/ui/dialog/select -H 'Content-Type: application/json' -d '{"action":"ok"}'
curl -s -X POST http://192.168.3.54:8088/api/system/enter_slot -H 'Content-Type: application/json' -d '{"slot":0}'
curl -s http://192.168.3.54:8088/api/system/tickets/<id>
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"<provider>","action":"activate"}'
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"<consumer>","action":"activate"}'
adb -s 192.168.3.54:5555 logcat -d -v time | grep 'Inotia4Qol' > docs/history/gamehost-narrow-b2-20260922/logcat-ports-migration.txt
```

## 实际输出

- world_facts 级联：deactivate ticket done → `inactive` + 5 消费者 `cascade_stopped`
  （EB/inventory_hooks 经 `deactivating` 在途归零）；activate + 逐个恢复后 5 消费者全部
  `enabled`（**quest 本次 `enabled` 且 error=None**，修复前 `failed`）。
- item_facts 级联：7 消费者（attribute_quality/autosell/craft_ui/custom_recipe/
  extension_bag/inventory_hooks/special_equip）恢复后全部 `enabled, error=None`。
- character_facts 级联：push_all/special_equip 恢复 `enabled`；simple_mode 回门禁关
  `installed_disabled`（正确语义）。全插件 21/22 `enabled`（唯一非 enabled 为
  simple_mode 门禁关正常态）。
- FATAL/SIGSEGV 0。

## Host 证据

54/54 通过（含 typed handler 幂等重注册用例）；`check_arch` 无新增违规；普通/
kernel-only/probe 三种构建成功；kernel-only 不含三 port 业务符号；`git diff --check` 通过。
