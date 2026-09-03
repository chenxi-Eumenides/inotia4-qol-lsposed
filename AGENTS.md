# 代理工作规则

## 开始前

- 项目总览和入口：`README.md`
- 当前代码架构与依赖规则：`docs/development/architecture.md`
- API 契约：`docs/reference/api-reference.md`
- 环境、构建和设备：`docs/guides/build-and-deploy.md`
- 当前待办：`docs/development/planning/backlog.md`
- 扩展背包范围和验收：`docs/development/features/extension-bag/control-plane.md`
- 当前重构计划：`docs/development/planning/refactor-plan.md`
- 历史方案仅供追溯：`docs/history/`

先阅读与任务直接相关的文档，再修改代码；文档已有结论不得重复试错。

## 目录边界

- `module/`：Android/NDK 源码。
- `module/AGENTS.md`：Android 工程构建和缓存规则。
- `scripts/`：开发、分析、数据和验证脚本。
- `tools/`：第三方工具本体，不放运行输出。
- `apk/`：输入 APK、解码产物和静态数据。
- `output/`：最终 APK 交付物。
- `archive/`：历史实验和不可作为当前实现依据的材料。
- `.tmp/`：按任务隔离的一次性临时文件，可清理；不得作为源码、正式脚本或交付物目录。
- 工具输出不得写入项目目录之外。
- Gradle 8.11.1 缓存位于隐藏目录 `.gradle/wrapper/dists/`；构建优先执行 `scripts/build-debug.sh`，不得仅凭 Wrapper 的本地 zip 报错判断 Gradle 缺失。
- Android 构建只能执行 `scripts/build-debug.sh` 或 `scripts/build-release.sh`；禁止直接执行 `./gradlew`、系统 `gradle` 或手写 Gradle 二进制路径。

## 修改约束

- 不使用 `as any`、`@ts-ignore` 或空异常处理；本项目 Kotlin/C++ 也必须保持类型和错误语义清晰。
- 既有 API 路径、HTTP 方法、成功响应和 `NativeBridge` external 方法名/签名默认冻结。
- `game_symbols.h` 是游戏版本常量唯一来源，禁止在域文件写裸 VMA/偏移。
- Controller 只做路由、参数解析和 Service 调用，不直接调用 `NativeBridge`。
- native 依赖方向为 bridge → feature/core、feature → core、core 不依赖 feature；禁止循环依赖。
- 扩展背包持有 `g_virtual_bag_mtx` 时，不得调用会触发缓存刷新的 `op_ok()`。
- 重构阶段只移动职责，不顺手修改无关业务逻辑。

## 验证要求

修改后按影响范围执行：

1. `git diff --check`。
2. 相关 host tests。
3. Android debug 构建。
4. API smoke 或对应真机验收。

每个阶段独立提交；不要将构建产物、日志、缓存和临时文件加入源码变更。

## `.tmp/` 临时文件规则

- 每个任务先创建独立目录，例如 `.tmp/<task-name>/`；禁止把日志、截图、反汇编、探针输出直接散落在 `.tmp/` 根目录。
- 只允许保存本次任务可重建的中间文件、诊断日志、截图和临时输入；源码、可复用脚本、第三方工具、APK 交付物和长期证据不得写入 `.tmp/`。
- 任务完成后立即清理该任务目录；确认不再需要时可清空 `.tmp/`，不得删除 `apk/`、`output/` 或 `archive/` 内容代替清理。
- `.tmp/` 已被 Git 忽略，不得使用强制添加将其纳入提交。
