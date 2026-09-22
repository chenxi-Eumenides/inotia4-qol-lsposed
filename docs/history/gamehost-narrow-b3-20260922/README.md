# VM-K4-03：阶段 4 批 3——extension_bag 写面从 QolGameHost 内化

2026-09-22。设备 `192.168.3.54:5555`；monster v29 NPatch 集成包。

## 本批改动

- 新增 `contracts/game_ports/extension_bag_write_port.h` 与
  `game/adapter_ports/extension_bag_write_port.{h,cpp}`：36 个 `extension_bag_write`
  函数（inventory 8 + equipment 7 + UI 21）从 QolGameHost 迁至 EB 私有 raw
  Adapter Port；B 类实现（动态符号 `draw_original_bag/items`、变体分支
  `create_confirmation`、条件恢复 `restore_confirmation`、serialize/load/create 校验）
  随薄包装一并搬运，语义逐行等价。
- port 仅允许 `extension_bag` descriptor 注入（registry 校验，其他插件被拒）。
- EB 全部调用点（game_ui_virtbag.cpp + 10 个 .inc）改经注入 port；
  `contracts/game_host.h` 删除 extension_bag_write 区（309-369），
  `host/game_host.cpp` 删除对应实现；QolGameHost 字段与静态断言同步。

## 锁与事务纪律保持

rulebook 锁序（锁内私有状态、锁外游戏调用）、R-13 popup 状态互斥、
R-44 自死锁约束、confirmation owner 状态机、变体分支与 callback 槽 owner 判定
均逐行保持；host 54/54（含 EB 事务/ownership/popup 既有 target）。

## 命令与实际输出

```bash
scripts/build-debug.sh
scripts/patch-apk.sh output/<最新 debug>.apk
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_npatched.apk
# 重启 + opEnabled 后：
curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"extensionBagEnabled":true}'
curl -s http://192.168.3.54:8088/api/system/features
curl -s -X POST http://192.168.3.54:8088/api/debug/extension_bag/enter_view -H 'Content-Type: application/json' -d '{"bag":6}'
curl -s -X POST http://192.168.3.54:8088/api/debug/extension_bag/select_bag -H 'Content-Type: application/json' -d '{"bag":6}'
curl -s -X POST http://192.168.3.54:8088/api/debug/extension_bag/exit_view -H 'Content-Type: application/json' -d '{}'
curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"extensionBagEnabled":false}'
adb -s 192.168.3.54:5555 logcat -d -v time | grep 'Inotia4Qol' > docs/history/gamehost-narrow-b3-20260922/logcat-eb-write-port.txt
```

## 实际输出

- 门禁开：EB `enabled` + **29 资源全 `live`**、degraded=false。
- 门禁关：`installed_disabled` + 26 free + 2 disabled + 1 live（与历史基线
  `2 disabled+26 free+1 live` 一致）。
- 写 port 通道错误语义（主菜单帧冻结期门禁重开的帧驱动排队、进档后生效；
  背包面板未开时 fail-closed）：`inventory panel not open`、`not in extension view`
  ——与 VM-P1-S2 记录的历史行为完全一致。
- FATAL/SIGSEGV 0；`git diff --check` 通过。

## 已知边界

VM-43（商店数量出售）、VM-P4-07（背包拖拽/合并等游戏内操作）、VM-D2-01..04
的深度行为卡未在本批重跑（需游戏内场景）；按既有卡口径沿用，
批 5 收口时统一重跑。host 54/54 + 门禁两态资源语义 + 错误语义一致性
构成本批调用通道迁移的验收证据。
