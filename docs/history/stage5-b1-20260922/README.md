# VM-K5-01：阶段 5 批 1——诊断路由 owner 与 smoke 基线

## 当前事实

- `routes.inc` 共 143 条路由；smoke 探测清单通过脚本启动时校验，与 routes.inc method/path 集合完全一致。
- `/api/health`、`/api/system/features`、`/api/system/resources`、`/api/system/tickets/{id}`、`/api/system/routes` 的 owner 均为 `core.api`。
- 前四条诊断路由 `op_gated=0、lease_required=1`；health 与 routes 为 `op_gated=0、lease_required=0`。
- core.api 停用时，`/api/system/routes` 明确返回 `{"ok":false,"error":"http registry inactive"}`，不再返回假成功空列表；激活后恢复 143 条注册路由。

## 已执行验证

### Smoke 基线

命令：

```bash
uv run python scripts/verification/smoke_all.py \
  http://192.168.3.54:8088 \
  scripts/verification/smoke_baseline_v0.7.13.json
```

实际输出：`total=143 pass=120 warn=23 fail=0`；基线 JSON 中 143 条记录均含
`method/path/status/body`，与 routes.inc 为 143/143，无遗漏、无额外项。warn 为既有
501 占位端点、405 路由匹配行为、quest/{id} 404，以及两个会改变原版选项 popup
生命周期的 core.ui 调试端点主动跳过；不是连接失败或异常泄漏。两个跳过项仍以
method/path/status=null/body 空记录进入基线，路由存在性由 routes.inc 校验和
/api/system/routes 覆盖。

基线文件：`scripts/verification/smoke_baseline_v0.7.13.json`。

五条诊断路由在基线中均为 HTTP 200，响应概要保存在对应 JSON 的 `body` 字段。

### core.api 生命周期

设备：`192.168.3.54:5555`，monster v29，最终 Debug LSPatch 集成包。

- 停用前：features 中 `core.api=enabled`；routes `registered=143、accepting=true`。
- `POST /api/op/features/lifecycle {"id":"core.api","action":"deactivate"}`：ticket 7。
- 停用后：health/features/resources/tickets 仍 HTTP 200；features 中 `core.api=inactive`；routes 明确返回 registry inactive 错误。
- `POST /api/op/features/lifecycle {"id":"core.api","action":"activate"}`：ticket 8。
- 恢复后：features 中 `core.api=enabled`；routes `registered=143、accepting=true`；screen 仍为 world。

完整机器观测 JSON：`docs/history/stage5-b1-20260922/core-api-lifecycle.json`。

### 其他验证

- Host CTest：58/58 通过，包含 `http_registry_tests` 的 lease 与 inactive registry 断言。
- `git diff --check`、`check_arch.py`、`check_layers.py`、`check_offsets.py`、`check_jni.py`、`check_resources.py`：通过。
- `scripts/build-debug.sh`：普通 Debug 构建通过，APK `inotia4_qol_lsposed_debug_2609221903_cdd16a732a96.apk`。
- `scripts/patch-apk.sh`：overhaul、original、monster v29 三包生成成功，3/3。

## 限制

- HTTP 面无法直接构造 route lease；lease 在途、拒绝新 lease、归零清表由 Host
  `test_http_registry.cpp` 覆盖。
- `core.ui` 的 `open-option`/`open-panel` 两个调试端点不纳入实际 smoke 调用；它们会
  改变原版 option popup 的生命周期，当前变体存在 native UI 崩溃风险。脚本保留两条
  路由记录并以 warn/null status 表示，后续由 core.ui 专项验收处理。
