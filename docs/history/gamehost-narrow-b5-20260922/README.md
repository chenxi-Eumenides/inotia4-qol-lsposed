# VM-K4-05：阶段 4 批 5——QolGameHost ABI major=2 与残余读面收口

## 当前事实

- `QolGameHost` ABI 为 major `2`、minor `0`；character/inventory/item extras 读面改由独立 Adapter Port 提供。
- `world_teleport` 通过 `QOL_PORT_CHOICE` 读取 `state_id`。
- item-count facade 的实现位于 `game/adapter_ports`，Host 只保留装配转发。
- Host 旧字段访问和未提供 game adapter 实现的 link 负例已加入 CTest。

## 已执行验证

- Host CTest：`58/58` 通过，包含两个预期失败负例。
- `git diff --check`：通过。
- `check_arch.py` 及 `check_layers.py`、`check_offsets.py`、`check_jni.py`、`check_resources.py`：通过。
- `scripts/build-debug.sh`：普通 Debug 构建通过，产出 arm64-v8a 与 armeabi-v7a APK。
- 修复 kernel-only 构建收口：Game Adapter ports 属于内核，不能随业务插件一起排除；`CMakeLists.txt` 现始终编译 adapter port 实现。
- `scripts/build-debug.sh -PqolKernelOnly=true`：通过；无业务 `plugin_descriptor` 符号。
- `scripts/build-debug.sh -PqolDeviceProbe=true`：通过；device probe provider/consumer 符号存在。
- 普通、kernel-only、device-probe 产物 strings 检查：通过；extras port 符号存在，kernel-only 不含业务插件 descriptor。
- `scripts/patch-apk.sh`：使用普通 Debug APK 成功生成 overhaul、original、monster v29 三个 LSPatch 集成包，`成功 3 / 失败 0`。
- 本轮产物：普通 Debug `783cc5d3057e`；kernel-only `843853bac8fe`；device-probe `1c0b3fc5e26c`。
- 三个集成包均通过 `apksigner verify`，APK Signature Scheme v2 有效。

## 验收限制

VM-43、VM-46、VM-P4-07、VM-D2 的本批真机行为日志尚未取得；按交付定义，本批状态为 `NOT_ACCEPTED`，Host、静态检查和 Debug 构建不能替代真机回归。
