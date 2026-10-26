# `save_backup` 重构设计

> 依据：`docs/refactor/plugin-architecture-guide.md`（分层、状态边界、线程契约、迁移 playbook）。
> 范围：只改 `module/app/src/main/cpp/plugins/save_backup/`；插件外部（测试 CMake、Kotlin、API 文档）
> 的改动必须先确认。为了在当前边界内落地，L1–L4 的实现以内联 impl 头收口，不新增测试 target 源。

## 1. 目标

把 2455 行 `feature.cpp` 单体拆成可测试的 L0–L6 分层，保持现有 HTTP/UI 行为与 `.qol.bak`/`.qol_save`
兼容不变。先完成结构迁移；线程命令化（HTTP 写接口票据化）作为独立决策批次。

## 2. 目标分层

```text
plugin / api / ui                 L6 装配、HTTP、JSON、面板
        ↓
     usecases                    L4 导出/列表/条目/导入/删除/自动备份
        ↓
   transaction                   L3 导入/删除/回滚/恢复
    ↓        ↓
persistence  port                L2 文件/目录/bundle；游戏存档端口
    ↓        ↓
   state → model / contracts     L1 上下文/快照；L0 结果/类型
bundle / backup_files            L0 纯逻辑
```

只允许高层 include 低层。`api/json_codec` 只 include `model`；`persistence` 不做游戏调用；`state` 不碰文件。

## 3. 目录与职责

```text
plugins/save_backup/
  plugin.cpp/.h                 L6 descriptor/lifecycle/provider/scheduler
  api/http_handler.cpp/.h       L6 HTTP 参数校验 + usecase 调用（目标）
  api/json_codec.h/.h           L6 typed result → JSON（唯一拼 JSON 的地方）
  usecases/backup_usecases.*    L4 用例（typed Outcome）
  usecases/slot_summary.*       L4 槽摘要（load_data + 明文解析）
  usecases/meta_backfill.*      L4 旧 meta 回填
  transaction/transactions.*    L3 导入/删除/回滚/trash 恢复
  persistence/persistence.*     L2 文件原语/目录/bundle 枚举（header-only impl）
  port/save_port.*              游戏 load/encrypt/free/stale 端口（header-only impl）
  state/state.*                 L1 上下文、map names、槽元数据快照（header-only impl）
  model/types.h                 L0 Outcome/错误码/请求响应/BackupFile/SlotSummary
  bundle.cpp/.h                 L0 现有纯逻辑
  backup_files.cpp/.h           L0/L2 快照收集
  ui/...                        L6 面板
```

## 4. 数据与错误

`model/types.h` 定义 `BackupError`、`ErrorInfo`、`Outcome<T>`、`BackupFile`、`ExportResult`、
`ListResult`、`EntryResult`、`ImportResult`、`DeleteResult`、`SlotSummary`。L4 返回 `Outcome<T>`；
L6 的 `json_codec` 负责把错误码映射为现有文案（`bad slot`、`backup not found`、
`backup checksum mismatch` 等），不改 HTTP body 形状。

## 5. 线程与锁

- 查询（list/entry/list_files/auto_entry）：任意线程，只读文件 + `state` 快照，不调用游戏。
- 写/游戏读取（export/import/delete/delete_slot/auto_backup/slot_summary）：仅游戏主线程。
  面板直接调用；HTTP 写接口的票据化是后续独立批次（需要 API/Kotlin/测试改动，先不改）。
- `state` 用 `inline shared_ptr` 发布不可变快照（context/map names/slot meta）。
- bundle 读改写（backfill/delete 竞争）用窄 `file_mutex`；slot 写用 `slot_operation` lease。
- 锁序：`slot_lease → file_mutex → store`；不用 `recursive_mutex`，不用 `try_lock` 做重入检测。

## 6. 迁移批次与状态

| 批次 | 内容 | 状态 |
|---|---|---|
| 0 | 设计文档 | 完成 |
| 1 | `model/types.h` + `Outcome` | 完成 |
| 2 | `persistence`（文件/目录/bundle 枚举） | 完成（独立 TU） |
| 3 | `state`（context/map names/slot meta 快照） | 完成（独立 TU） |
| 4 | `port`（load/encrypt/free/stale） | 完成（独立 TU） |
| 5 | `transaction`（导入/删除/回滚/恢复） | 完成（独立 TU） |
| 6 | `usecases` + `api/json_codec` + `feature.cpp` 薄 facade | 完成（`feature.cpp` 40 行） |
| 7 | HTTP 写路径票据化（`post_typed`/`read_result`） | 完成：native 3 条写端点投递 typed command；Kotlin `postAndAwait` 轮询并保持对外同步 JSON |
| 8 | 面板直接消费 typed result、删除 JSON 解析 | 完成：`ui/panel.cpp` 已改用 typed usecase，移除 3 个 JSON 解析器 |
| 9 | 回归：host gate + Android | host/构建已过；按策略不做真机验证 |

## 6.1 批次 7 已批准的插件外改动

| 文件 | 改动 | 原因 |
|---|---|---|
| `module/app/src/main/java/com/inotia4/qol/api/controller/SaveController.kt` | 写端点（export/import/delete）改为 `post_typed` 票据回取，对外保持原 JSON 形状 | HTTP 写路径必须回主线程；公共 API 不变 |
| `docs/reference/api-reference.md` | 记录写端点票据语义（或由 Kotlin 适配层完全隐藏） | 契约同步 |
| `module/app/src/main/cpp/tests/CMakeLists.txt` | 已把新层源文件加入 `save_backup_gate_tests` | 独立 TU 方案 |
| `module/app/src/main/cpp/tests/test_save_backup_gate.cpp`（或新 handler test） | 增加 HTTP 票据/参数/失败回滚用例 | L6 覆盖 |
| `docs/refactor/api-route-map.md` | 若票据化改变端点入口，更新映射 | 文档同步 |

真机验证按“禁止多代理同时使用一台真机”策略，不纳入本轮；不涉及文件改动。

> 当前实现约束：为遵守“只改插件区域、不改测试 CMake”的边界，L1–L4 均以 header-only impl 收口；
> `transaction_impl` 在 usecases TU 内展开并复用其日志/错误适配。后续如允许新增测试 TU，
> 可把 impl 头拆成独立 `.cpp` 并去掉 header-only 聚合。

## 7. 验证

- `save_backup_gate_tests`：304/304 通过（含 L6 typed command payload/票据 ACK 契约）。
- `host_tests`：2791 passed / 0 failed。
- `check_arch`：layers/offsets/jni/resources 全绿；`check_jni` 0 违规。
- `git diff --check`：通过。
- `scripts/build-debug.sh`：Debug APK `output/inotia4_qol_lsposed_debug_2610252148_2639f9a8d342.apk`（SHA-256 `2639f9a8d3425c3e06e18217ae08dc3c9b77d39b14faa6812e62cdcca0d37bcb`）。
- 真机：按“禁止真机验证、多代理不得同时使用一台真机”策略，本轮不做真机验证，也不作为完成条件；后续由单一授权代理按需执行。
- 说明：`check_http_routes` 当前仅报工作区其他代理未提交的 `POST /api/extension_bag/set_bag_count`；`save_backup` 仍为 4 条路由。

## 8. 不做

- 不重写 `bundle.cpp`；不引入通用 DI 容器；不把业务规则 JSON 化。
- 不在批次 1–6 改 HTTP JSON 形状；线程票据化单独批准。
- 插件外改动已按用户批准执行：`tests/CMakeLists.txt`（新层源文件）、`SaveController.kt`（票据回取）、API/route 文档；未改动其他代理区域。
