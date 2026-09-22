# VM-K4-01：阶段 4 批 1——Adapter Port + typed command seam 试点（quest）

2026-09-22。设备 `192.168.3.54:5555`；monster v29 NPatch 集成包。

## 本批改动

- 新增 `contracts/game_ports/ports.h`（port id 枚举）与 `quest_port.h`（`QolQuestPort` v1.0，
  standard_layout/trivially_copyable 静态断言）。
- 新增 `game/adapter_ports/quest_port.cpp`：原 `host/game_host.cpp` quest 读写包装迁入，
  直接调 `game/access/quest.h`。
- `contracts/plugin.h`：PluginContext 尾部追加 `ports`/`port_count`（`PLUGIN_CONTEXT_STRUCT_SIZE`
  兼容）；PluginDescriptor 尾部追加 `ports`/`port_count`（默认 nullptr，旧初始化天然兼容）。
- `host/plugin_registry.cpp`：descriptor port 声明解析、版本校验（major 相等 minor 满足）、
  注入 PluginContext；缺失/不匹配 → `unsupported`（fail-closed，错误含 port id）。
- `infra/commands`：typed command（固定 POD payload、复制入队、主线程执行、handler 注册/注销）。
- `plugins/quest`：声明 `QOL_PORT_QUEST` v1.0；读经 port；`set_state` 写经自有 typed command
  handler；`contracts/game_host.h` + `host/game_host.cpp` 删除 quest 读区与 quest_write 区。

## 命令与实际输出

```bash
scripts/build-debug.sh
scripts/patch-apk.sh output/<最新 debug>.apk
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_npatched.apk
# 重启后：
curl -s http://192.168.3.54:8088/api/health
curl -s -X POST http://192.168.3.54:8088/api/config/set -H 'Content-Type: application/json' -d '{"opEnabled":true}'
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"quest","action":"deactivate"}'
curl -s -X POST http://192.168.3.54:8088/api/ui/dialog/select -H 'Content-Type: application/json' -d '{"action":"ok"}'
curl -s -X POST http://192.168.3.54:8088/api/system/enter_slot -H 'Content-Type: application/json' -d '{"slot":0}'
curl -s http://192.168.3.54:8088/api/system/tickets/7
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"quest","action":"activate"}'
adb -s 192.168.3.54:5555 logcat -d -v time | grep 'Inotia4Qol' > docs/history/gamehost-narrow-b1-20260922/logcat-quest-port.txt
```

实际输出：`/api/health` ok+ready；quest 与三个 facts provider 全部 `enabled`；deactivate
ticket 7 `done` → `quest inactive`；activate → `quest enabled`；日志锚
`quest resync: fixed=0`（f=126，经 port 的读路径）；FATAL/SIGSEGV 0。

## Host 证据

54/54 通过，含新增：fake quest port 版本匹配注入、版本不匹配/缺失 → unsupported、
typed command payload 复制/主线程执行/handler 注册注销/超限拒绝、quest 经 port 读写装配。
`check_arch` 全部通过、无新增违规；普通与 kernel-only 构建成功，kernel-only 不含 quest 业务符号。
