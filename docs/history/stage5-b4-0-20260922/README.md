# VM-K5-04a：阶段 5 批 4-0——HTTP dispatch 接缝与 `game_frame` 试点

## 变更范围

- `HttpReq` 尾部追加 `query` 与 query 长度；新增只读 query handler、预留 command handler、dispatch 和 owner 注销契约。
- HTTP registry 使用 mutex/condition variable 保护路由与 lease；保留 `unregister_owner(nullptr)` 的 inactive/timeout 语义，owner 注销等待其路由 lease 归零。
- 新增模板路由匹配：`{param}` 仅作为完整路径段，handler 可读取 `HttpPathParam`；重复绑定、handler 缺失、owner 停用和 registry inactive 均返回明确错误。
- 将 dispatch API 以 `QolHostSdk.http_dispatch` 尾部追加方式注入，`QolHostSdk` minor 从 6 升至 7；既有 registry 字段和既有 JNI external 签名不改。
- command dispatch 仅保留契约形态，本批返回 `unsupported`，不新增 queue。
- `/api/system/game_frame` 作为无参数只读试点改走 `nativeHttpDispatch`；HTTP 方法、路径、状态码和成功 body 维持原值。

## R-xx / B-xx 影响

- 本批无新增或改变的 R-xx / B-xx 编号。
- 受影响 VM 卡：`VM-K5-04a`；既有 `game_frame` 成功响应行为由本卡复验，其他业务行为沿用 `VM-K5-01`、`VM-K5-02`、`VM-K5-03` 与既有行为卡。

## 验证结果

- `git diff --check`：通过。
- Host CTest：`100% tests passed out of 59`；新增 `http_dispatch_tests`（`pass=23 fail=0`）覆盖模板参数、query、重复绑定、command unsupported、owner 注销等待、handler 缺失和 inactive。
- `uv run python scripts/maintenance/check_http_routes.py`：143/143，重复 0，controller/native/api-reference 差异 0。
- `uv run python scripts/maintenance/check_jni.py`：Kotlin external/native 导出 `145/145`，违规 0。
- `uv run python scripts/maintenance/check_arch.py`：全部通过，无新增架构违规。
- `uv run python scripts/maintenance/gen_baseline_doc.py`：通过，已更新 `docs/refactor/baseline.md` 的当前 JNI/层分布基线。
- `scripts/build-debug.sh`：`BUILD SUCCESSFUL`，APK：`output/inotia4_qol_lsposed_debug_2609222318_39826f68e1a3.apk`。
- 真机 `192.168.3.54:5555`：安装 Debug APK、force-stop、monkey 重启；`/api/health` 为 `monster/v29/ready=true`；`/api/system/game_frame` 返回 `{"frame":413}`；`/api/system/routes` 为 143 条且 `duplicates=0`、`omitted=0`、`accepting=true`。
- 全量 smoke：`total=143、pass=120、warn=23、fail=0`；`game_frame` 为 `HTTP 200`；logcat 未发现 `FATAL`、`SIGSEGV` 或 `UnsatisfiedLinkError`。

证据文件：本目录的 `build-debug.log`、`host-ctest.log`、`http-dispatch-tests.log`、`http-registry-tests.log`、`check-http-routes.log`、`check-jni.log`、`check-arch.log`、`gen-baseline.log`、`health.json`、`routes.json`、`game-frame.json`、`smoke.log`、`logcat.txt`。
