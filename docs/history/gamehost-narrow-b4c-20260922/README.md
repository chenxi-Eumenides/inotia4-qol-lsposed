# VM-K4-04c：阶段 4 批 4c——craft 读面与 owner 写面收口

2026-09-22。设备 `192.168.3.54:5555`；monster v29 LSPatch 集成包
（模块 APK `inotia4_qol_lsposed_debug_2609221458_4f3947c2291a.apk`）。

## 构建与安装

```bash
scripts/build-debug.sh -PqolKernelOnly=true
scripts/build-debug.sh -PqolKernelOnly=true -PqolDeviceProbe=true
scripts/build-debug.sh
scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609221458_4f3947c2291a.apk
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_lspatched.apk
```

- 普通、kernel-only、device-probe Debug 构建均成功。
- 集成包生成 3/3 成功；当前环境脚本使用 LSPatch v1.2，输出后缀为
  `_lspatched`。
- 启动后 `/api/health` 返回 `ok=true, ready=true`，同意页关闭后进入
  `main_menu`，`enter_slot` 后进入 `world`。

## 已取得证据

- 启动装配：`craft_facts` 为 `enabled`，发布 `craft.facts` v1.0；
  `craft_ui` 与 `custom_recipe` 均可恢复为 `enabled`，声明资源均为 `live`。
- 级联：停用 `craft_facts` 后，`custom_recipe` 进入
  `installed_disabled/cascade_stopped`，`craft_ui` 进入异步
  `deactivating`；重新启用 provider 后，两个消费者可手动恢复。
- 生命周期票据：craft provider、craft UI、custom recipe 的 deactivate/activate
  请求均返回成功票据。

## 编排方复验（2026-09-22，最终验收口径）

重启后（普通变体确认，kernel-only 日志 0）：

- 门禁关基线：`craft_facts enabled`（provides `craft.facts`）；craft_ui
  `installed_disabled` + degraded=`missing optional capability: craft_result`
  （custom_recipe 门禁关时 optional 缺失，与批 2 前历史行为一致）。
- `customRecipeEnabled=true` + 进档推进帧后：**custom_recipe `enabled`（1 资源 live）、
  craft_ui `enabled`（12 资源 live）、degraded/error 全 None**——与 VM-46 门禁语义一致。
- fixer 初验报告的两个观察项已澄清：① craft_ui degraded=门禁关时 optional 缺失的
  历史一致行为；② 配置切换"未生效"=主菜单帧冻结（f=118）的帧驱动语义，进档后落地。
- FATAL/SIGSEGV 0（`logcat-craft-facts.txt` 追加段）。

结论：**VM-K4-04c PASS（Debug 真机，调用通道+门禁口径）**；VM-46 深度行为卡
（三格合成游戏内操作）未重跑，批 5 统一执行。
- 最终 logcat：未发现 `FATAL EXCEPTION`、`SIGSEGV`、`Fatal signal` 或
  `Abort message`。
- 证据文件：`features-final.json`、`resources-final.json`、`logcat-craft.txt`、
  `logcat-config-filtered.txt`、`fatal.txt`。

## 未闭合项

- VM-46 的宝石强化页、三格放料、合成扣料/产物、混档/混沌提示、关配置原版
  配方与反复 toggle 尚未完成，因此本卡仍为 `NOT_ACCEPTED`。
- 本轮最终状态显示 `craft_ui.degraded=true`；原因是启动阶段
  `craft_result` 尚未发布时记录了 optional capability 缺失，后续 provider 发布
  后该健康标记未清除。资源本身均为 `live`，但该状态需单独处理或确认语义。
- `/api/config/set` 返回 `customRecipeEnabled=false/true`，但本轮查询中
  `custom_recipe` 生命周期未随配置请求切换；需在后续行为回归中确认配置门禁路径。
