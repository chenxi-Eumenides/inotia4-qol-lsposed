# VM-K4-04a：阶段 4 批 4a——choice 领域内化到 native_choice owner

2026-09-22。设备 `192.168.3.54:5555`；monster v29 NPatch 集成包
（模块 APK `inotia4_qol_lsposed_debug_2609221322_1efb0efc3b1d.apk`）。

## 本批改动

- 新增 `contracts/game_ports/choice_port.h`（`QolChoicePort` v1.0：choice 读 3 函数 +
  choice_action 写 6 函数）与 `game/adapter_ports/choice_port.{h,cpp}`（薄包装搬运，
  直接调 `game/access/ui_choice.h`）。
- `plugins/native_choice`：descriptor 声明 `QOL_PORT_CHOICE` v1.0，install 经 ctx.ports
  注入（fail-closed），读写调用点改经 port；`ui_choice` capability 与消费者
  （world_teleport）完全不变。
- `contracts/game_host.h` + `host/game_host.cpp` 删除 choice 读区与 choice_action 写区。
- 已知残留：`world_teleport/feature.cpp` 保留一处 `state_id` 直读（choice 读区该函数
  未删除），随批 4b/4c 的 world_teleport 迁移处理。

## 命令与实际输出

```bash
scripts/build-debug.sh
scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609221322_1efb0efc3b1d.apk
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_npatched.apk
# 重启后：
curl -s http://192.168.3.54:8088/api/health
curl -s http://192.168.3.54:8088/api/system/features
adb -s 192.168.3.54:5555 logcat -d -v time | grep 'Inotia4Qol' > docs/history/gamehost-narrow-b4a-20260922/logcat-choice-port.txt
```

实际输出：health ok+ready；`native_choice enabled`（2 资源 live，provides `ui_choice`）、
`world_teleport enabled`（1 资源 live）；kernel-only 日志 0 条（普通变体确认）；
FATAL/SIGSEGV 0。

## Host 证据

54/54 通过（含 fake choice port 注入/版本不匹配 unsupported）；`check_arch` 无新增违规；
普通/kernel-only/probe 三种构建成功；strings 确认普通含 `choice_port`、两种 kernel-only
构建不含；`git diff --check` 通过。
