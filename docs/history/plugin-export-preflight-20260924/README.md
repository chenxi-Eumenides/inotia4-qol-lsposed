# 插件导出符号目标与安装前预检真机验收

日期：2026-09-24

## 验收范围

设备：`192.168.3.54:5555`；游戏：monster v29，variant 日志确认 `md5=4c332e9b508b35fe1e97f1f26e56f04d`、`known=1`。

本记录包含早期无效 hook 观测、修复后正向符号 hook 真机验收、必需 fail-closed 缺失导出结果及普通 Debug 恢复证据。09:17 的 `core.ui_state` 独立 STATIC target 未收到 `QOL_ENABLE_DEVICE_PROBE` private 宏，`UIChoice_Init` 实际仍走旧 `F_UI_CHOICE_INIT_VMA`；该次 `live → free → live` 不能作为新导出目标物理安装/还原的证据。用户批准修复后，target 级宏定义进入重新构建，10:43 真机日志及 APK 内字符串确认新 probe 确实采用 `elf-export:UIChoice_Init`，并补齐该 hook 的安装/停用/恢复闭环。此结论不否定 `GRPX_Start` 正向只读解析、required + fail-closed 缺失导出负向路径或宿主隔离结果。负向 probe 仅 Debug 构建启用，不进入 Release；未执行 Release 构建或 Release 隔离验收。未知导出与已有资源是否发生同址冲突仲裁尚未通过真机验证，作为风险保留；fake 后端的冲突仲裁 Host 覆盖见下文。

## 构建与产物

每次测试变体都先用 debug 脚本构建 device probe，再把对应模块 APK 显式交给集成脚本；独立 LSPosed 模块安装不会更新真机上 LSPatch 包内嵌的模块。正向构建/集成命令：

```sh
./scripts/build-debug.sh --offline -PqolDeviceProbe=true
MODULE_APK="output/<替换为上一步打印的 Generated Debug APK filename>"
./scripts/patch-apk.sh "$MODULE_APK"
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_lspatched.apk
adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common
adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1
curl -s http://192.168.3.54:8088/api/health
```

早期 probe APK `output/inotia4_qol_lsposed_debug_2609240916_74545cf8bb6c.apk`，SHA-256 为 `74545cf8bb6c6f4fece3ff20b70d7a063b38b2d4c816f19b75daaa4a9e046a09`；它保留为历史正向只读解析/旧 hook 实验产物，不作为修复后 hook 验收产物。执行 `sha256sum output/inotia4_qol_lsposed_debug_2609240916_74545cf8bb6c.apk` 的历史实际输出：

```text
74545cf8bb6c6f4fece3ff20b70d7a063b38b2d4c816f19b75daaa4a9e046a09  output/inotia4_qol_lsposed_debug_2609240916_74545cf8bb6c.apk
```

09:12 最早 positive probe build 的 SHA-256 前缀记录为 `53c388a4…`。09:17 的资源状态变化沿用上列旧路径，不支持新 hook 结论，见下文更正。修复后验收必须从当前源码重新构建、将该次生成的模块 APK 传给 `scripts/patch-apk.sh`，再安装新生成的 LSPatch 集成包；单独安装 LSPosed 模块不会更新集成包内嵌模块。

## 早期正向解析记录与 09:17 无效 hook 物理路径观测

09:17 的正向测试先安装更新后的 monster v29 LSPatch 集成包、重启游戏、关闭协议同意页并进档至 world，然后依次执行 deactivate/activate。重跑时先按上一节用对应模块 APK 重建集成包，再执行：

```sh
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_lspatched.apk
adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common
adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1
curl -s http://192.168.3.54:8088/api/health
curl -s -X POST http://192.168.3.54:8088/api/ui/dialog/select -H 'Content-Type: application/json' -d '{"action":"ok"}'
curl -s -X POST http://192.168.3.54:8088/api/system/enter_slot -H 'Content-Type: application/json' -d '{"slot":0}'
curl -s http://192.168.3.54:8088/api/system/tickets/<本次enter_slot返回的ticket>
curl -s http://192.168.3.54:8088/api/ui/screen
curl -s http://192.168.3.54:8088/api/system/features
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"core.ui_state","action":"deactivate"}'
curl -s http://192.168.3.54:8088/api/system/tickets/<本次deactivate响应返回的ticket>
curl -s http://192.168.3.54:8088/api/system/features
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"core.ui_state","action":"activate"}'
curl -s http://192.168.3.54:8088/api/system/tickets/<本次activate响应返回的ticket>
curl -s http://192.168.3.54:8088/api/system/features
```

以下为实际结果摘录：

```text
09:12:41.022 device_probe: domain=elf_export symbol=GRPX_Start result=hit expected=hit followed=false
09:12:41.023 device_probe: domain=elf_export symbol=QOL_Preflight_Missing_Export result=missing expected=missing followed=false
09:12:41.348 assembly: registered=26 install_failed=0 kernel_only=false
```

09:17:35 的设备日志及 API 实际记录为：同意页关闭、ticket 7 进档到 `world`，随后 `core.ui_state` deactivate（ticket 8 `done`，插件 `inactive`，hook/ptrslot 均 `free`），再 activate（ticket 9 `done`，插件 `enabled`，hook/ptrslot 均 `live`）。此为真实历史输出，但因 target 未收到 probe 宏，实际仍使用旧 `F_UI_CHOICE_INIT_VMA`，不证明新导出目标解析或物理安装。

## 修复后导出 hook 真机验收

当前源码的复跑入口（使用构建脚本实际打印的模块 APK 文件名，不依赖轮转后旧 APK）：

```sh
./scripts/build-debug.sh --offline -PqolDeviceProbe=true
MODULE_APK="output/<本次build-debug.sh打印的Generated Debug APK文件名>"
unzip -p "$MODULE_APK" lib/arm64-v8a/libgamebridge.so | strings | rg 'elf-export:UIChoice_Init'
./scripts/patch-apk.sh "$MODULE_APK"
apksigner verify "$MODULE_APK"
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_lspatched.apk
adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common
adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1
curl -s http://192.168.3.54:8088/api/health
```

修复后正向 probe 模块 APK：`output/inotia4_qol_lsposed_debug_2609241038_b91004de3378.apk`，SHA-256 为 `b91004de337818dac5923f593b5ee9eb058687374b4cdd7c61b299a607e4a10d`。执行 `sha256sum output/inotia4_qol_lsposed_debug_2609241038_b91004de3378.apk` 的实际输出：

```text
b91004de337818dac5923f593b5ee9eb058687374b4cdd7c61b299a607e4a10d  output/inotia4_qol_lsposed_debug_2609241038_b91004de3378.apk
```

该 APK 中 `unzip -p ... lib/arm64-v8a/libgamebridge.so | strings | rg 'elf-export:UIChoice_Init'` 实际命中 `elf-export:UIChoice_Init`；`rg -n '"elf-export:UIChoice_Init"' module/app/src/main/cpp/plugins` 实际只命中 `plugins/core.ui_state/plugin.cpp` 的 Debug 条件分支。该 target 当前编译定义为 `QOL_ENABLE_DEVICE_PROBE=$<BOOL:${QOL_ENABLE_DEVICE_PROBE}>`（PRIVATE）。源码条件分支与最终二进制字符串共同证明这次构建启用了自声明导出目标。`apksigner verify "$MODULE_APK"` 退出码 `0`；`scripts/patch-apk.sh "$MODULE_APK"` 实际汇总 `成功 3 / 失败 0`。

monster v29 游戏 MD5 为 `4c332e9b508b35fe1e97f1f26e56f04d`。修复后的设备日志实际摘录：

```text
10:43:37.909 game_variant: series=monster version=v29 caps=0x3 md5=4c332e9b508b35fe1e97f1f26e56f04d known=1
10:43:37.964 device_probe: domain=elf_export symbol=GRPX_Start result=hit expected=hit followed=false
10:43:37.965 device_probe: domain=elf_export symbol=QOL_Preflight_Missing_Export result=missing expected=missing followed=false
10:43:38.383 assembly: registered=27 install_failed=1 kernel_only=false
10:43:38.383 device_probe: plugin=device_probe_export_failure state=unsupported error=required entry unresolved
10:43:38.383 assembly: initialized=true registered=27 install_failed=1 kernel_only=false
```

同次 `/api/system/features` 实际观察：`core.ui_state=enabled`，`hook:UIChoice_Init=live`、对应 ptrslot=`live`；`device_probe_export_failure=unsupported` 且缺失资源 `free`；`device_probe_provider`/`device_probe_consumer` 均 enabled。进入 `world` 后的生命周期命令顺序为先 deactivate、再 activate：

```sh
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"core.ui_state","action":"deactivate"}'
curl -s http://192.168.3.54:8088/api/system/tickets/<本次deactivate响应返回的ticket>
curl -s http://192.168.3.54:8088/api/system/features
curl -s -X POST http://192.168.3.54:8088/api/op/features/lifecycle -H 'Content-Type: application/json' -d '{"id":"core.ui_state","action":"activate"}'
curl -s http://192.168.3.54:8088/api/system/tickets/<本次activate响应返回的ticket>
curl -s http://192.168.3.54:8088/api/system/features
curl -s http://192.168.3.54:8088/api/ui/screen
```

本次实际结果：deactivate ticket 8 `done` → `inactive`、hook/ptrslot 均 `free`；activate ticket 9 `done` → `enabled`、hook/ptrslot 均 `live`；screen 持续为 `world`。8/9 是该次实测编号，复跑必须按 POST 实际返回的 ticket 轮询。此真机证据支持修复后的导出目标物理安装/还原，但不覆盖原生选择框视觉交互、新游戏版本、ARM32 或 Release。

同址碰撞仲裁由 Host fake 后端用例验证：同插件两个不同导出 ID 解析到同址时插件在 hook/install 前拒绝且 hook 调用数为 0；跨插件第二 owner 被拒，首 owner 保持 live；资源 release 后地址可复用；tombstone 状态拒绝复用。完整 Host CTest `100% tests passed, 0 tests failed out of 66`。这些是 Host 假后端结果，不代表真机发生过碰撞；真机仅验证正常目标安装和恢复。`check_arch` 实际 `全部通过`、新增违规 `0`。

## 缺失导出 fail-closed 预检

09:33:48 的负向测试模块 APK 为 `output/inotia4_qol_lsposed_debug_2609240932_7092a41ee775.apk`，SHA-256 为 `7092a41ee7756ba4921fb4fe8c80612c085272ddee9a940fe678e9347e27e875`。执行 `sha256sum output/inotia4_qol_lsposed_debug_2609240932_7092a41ee775.apk` 的实际输出：

```text
7092a41ee7756ba4921fb4fe8c80612c085272ddee9a940fe678e9347e27e875  output/inotia4_qol_lsposed_debug_2609240932_7092a41ee775.apk
```

使用该负向配置重建时同样先设置 `MODULE_APK` 为本次 `build-debug.sh` 打印的 APK 路径，再执行 `scripts/patch-apk.sh "$MODULE_APK"`，安装新生成的 `output/Inotia4_v1.3.2_monster_v29_lspatched.apk`、force-stop + monkey 重启并轮询 `/api/health`，不能只安装独立模块 APK。已记录的负向测试模块文件为 `output/inotia4_qol_lsposed_debug_2609240932_7092a41ee775.apk`。09:33:48 的 Debug-only `device_probe_export_failure` 声明 required + fail-closed 目标 `elf-export:QOL_Preflight_Missing_Export`。设备实际日志摘录：

```text
09:33:48.617 assembly: registered=27 install_failed=1 kernel_only=false
09:33:48.617 device_probe: plugin=device_probe_export_failure state=unsupported error=required entry unresolved
09:33:48.625 NativeBridge init OK
```

对应资源 `hook:device_probe:missing_export=free`；独立 `core.ui_state`、`core.api`、`extension_bag` 和旧 probe provider/consumer 均保持 enabled。`GET /api/health` 返回 `ready=true`、variant `monster/v29`；随后从 main_menu 关闭同意页并成功进入 world。日志中 `NativeBridge init OK` 是实际可见行；其长行受 Android 日志截断，`initialized=true` 字段未能从 logcat 观察到，因此本记录不声称观测到该字段或 report 内容。

## 普通 Debug 恢复验收

负向 probe 验收后构建不含 probe 的普通 Debug，并重新生成/安装 monster v29 LSPatch 集成包。复跑命令：

```sh
./scripts/build-debug.sh
sha256sum output/inotia4_qol_lsposed_debug_2609240938_97099e721998.apk
./scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609240938_97099e721998.apk
adb -s 192.168.3.54:5555 install -r output/Inotia4_v1.3.2_monster_v29_lspatched.apk
adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common
adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1
curl -s http://192.168.3.54:8088/api/health
curl -s http://192.168.3.54:8088/api/system/features
curl -s http://192.168.3.54:8088/api/system/routes
curl -s -X POST http://192.168.3.54:8088/api/ui/dialog/select -H 'Content-Type: application/json' -d '{"action":"ok"}'
curl -s -X POST http://192.168.3.54:8088/api/system/enter_slot -H 'Content-Type: application/json' -d '{"slot":0}'
curl -s http://192.168.3.54:8088/api/system/tickets/<本次enter_slot返回的ticket>
curl -s http://192.168.3.54:8088/api/ui/screen
```

该普通 Debug APK 的实际 SHA-256：

```text
97099e7219985712e453b5a3bf60dc962fb61f7d89b7eb8990a2ef0565d40fc6  output/inotia4_qol_lsposed_debug_2609240938_97099e721998.apk
```

实际结果摘录：

```text
scripts/patch-apk.sh: 成功 3 / 失败 0
/api/health: {"ok":true,"ready":true,"variant":{"series":"monster","version":"v29","known":true,"capabilities":3}}
/api/system/features: 25 条；probe_ids=[]；core.ui_state=enabled，hook:UIChoice_Init=live，ptrslot=live
/api/system/routes self_check: registered=143 expected=143 duplicates=0 omitted=0 accepting=true
enter_slot: ticket=7 done；/api/ui/screen: world
```

普通 APK 的 probe 符号隔离复核命令及实际输出：

```sh
unzip -p output/inotia4_qol_lsposed_debug_2609240938_97099e721998.apk lib/arm64-v8a/libgamebridge.so | strings | rg 'device_probe_export_failure|QOL_Preflight_Missing_Export'
```

实际输出：无匹配（`rg` 退出码 `1`）。对对应负向 Debug probe APK 执行同一检查：

```sh
unzip -p output/inotia4_qol_lsposed_debug_2609240932_7092a41ee775.apk lib/arm64-v8a/libgamebridge.so | strings | rg 'device_probe_export_failure|QOL_Preflight_Missing_Export'
```

实际结果：有匹配，含 `device_probe_export_failure` 与 `QOL_Preflight_Missing_Export` 标记。

由此仅确认该普通 Debug 构建未包含上述 Debug probe 标记，且 monster v29 集成包恢复 ready、插件/路由自检正常并能进 world；普通 Debug 恢复事实不扩展 VM-EXPORT-01 对符号 hook 物理安装的覆盖范围，也不构成 Release 隔离验收。

## 其他已执行验证

- Host CTest 复跑（使用项目内构建目录变量，不依赖已生成的构建树）：

  ```sh
  PROJECT_ROOT="$(git rev-parse --show-toplevel)"
  HOST_BUILD_DIR="${PROJECT_ROOT}/module/app/src/main/cpp/tests/build"
  cmake -S "${PROJECT_ROOT}/module/app/src/main/cpp/tests" -B "${HOST_BUILD_DIR}"
  cmake --build "${HOST_BUILD_DIR}"
  ctest --test-dir "${HOST_BUILD_DIR}" --output-on-failure
  ```

  本次实际 Host 输出：`100% tests passed, 0 tests failed out of 66`。
- 架构：`uv run python scripts/maintenance/check_arch.py`；实际输出 `全部通过`，新增违规 `0`。
- 修复后 Debug probe 构建：`./scripts/build-debug.sh --offline -PqolDeviceProbe=true`；对应输出 `output/inotia4_qol_lsposed_debug_2609241038_b91004de3378.apk`，实际日志 `BUILD SUCCESSFUL in 19s`。
- 修复后集成：`./scripts/patch-apk.sh output/inotia4_qol_lsposed_debug_2609241038_b91004de3378.apk`；本次集成日志实际汇总 `成功 3 / 失败 0`。对修复后 probe APK 执行 `apksigner verify` 退出码 `0`。

上述证据不覆盖 Release 构建、Release probe 隔离、其他变体/架构、任意第三方 ABI 或原生选择框视觉/行为。普通 Debug 恢复仅限上述 monster v29 Debug 集成构建。修复后的新导出 hook 通过该 monster v29 Debug 机制验收；同址冲突证据仅来自 Host fake backend，真机未验证碰撞。依据项目证据规则，本记录内联保留复跑命令与相应实际结果；不将临时诊断文件路径作为正式证据引用。
