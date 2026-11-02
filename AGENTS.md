## 文档使用

- 文档目录和用途见 `docs/INDEX.md`。
- 目标架构以根目录 `ARCHITECTURE.md` 为准；API 契约以 `docs/reference/api-reference.md` 为准；重构状态和验收口径以 `docs/refactor/` 为准。
- 开始修改前查阅受影响领域的权威资料；已有文档结论不得重复探索或试错。
- 有疑问时先查相关文档，仍未解决再询问。

## 目录边界

- `module/`：Android/NDK 源码。
- `module/AGENTS.md`：Android 工程规则入口，构建、部署与设备验收流程见 `docs/guides/build-and-deploy.md`。
- `module/app/src/main/cpp/AGENTS.md`：Native 源码职责与局部修改规则。
- `scripts/`：开发、分析、数据和验证脚本。
- `tools/`：第三方工具本体和静态工具页面，不放运行输出。
- `apk/`：输入 APK 与静态数据。`game-apk/` 为输入包（`history/` 为历史版本），`decoded/` 为 apktool 解码结果（smali/res/assets/lib），`decompiled/` 为 jadx 反编译结果（sources/resources）；`static-data/` 顶层为 `json/` 与 `raw/`，`json/` 下按 `tables/`、`text/`、`maps/`、`snasys/` 分类。
- `output/`：最终 APK 交付物。
- `archive/`：历史实验和不可作为当前实现依据的材料。
- 工具输出不得写入项目目录之外。
- **Android 构建入口**：日常开发与验证只允许 `scripts/build-debug.sh`；`scripts/build-release.sh` 仅在用户明确要求 release 时使用；禁止直接执行 `./gradlew`、系统 `gradle` 或手写 Gradle 二进制路径。

## 目录规范与环境隔离（强制）

- **工具输出必须项目内**：凡工具会产出文件——解码和反编译结果、构建产物、解析数据、日志与截图——输出路径必须落在项目目录内，不得写入系统目录或项目外临时目录。
- **Python**：项目依赖一律用项目内 `.venv/`（`uv run`），禁止向系统 Python 安装项目依赖；系统 pacman 的 python-frida 不用于本项目。
- **只读环境依赖**：Android SDK（`/opt/android-sdk`）属运行环境，仅引用，项目文件不写入其中。
- **提交（强制）**：一个功能一个提交，提交信息描述该功能；**提交时不得变更版本号**（版本号只在 release 流程中变更）。功能只有代码已提交后才算完成。
- **Release**：仅在用户明确要求时执行，详细流程见 `docs/guides/build-and-deploy.md` §3.2；发布说明经用户确认后才推送和创建 Release。
- **Build**: 仅允许运行构建、打包脚本，其清理行为是正确行为。不得手动构建、打包，不得搬迁旧版本。

## 修改约束

- 保持类型安全和明确的错误语义，不使用不安全的类型绕过或空异常处理。
- 代码注释只写事实与原因：当前做法的原因、不能采用另一种做法的原因必须保留；决策过程、时间戳、「评审 / 裁决 / 用户决定」这类来源标记不写入注释。
- 改动说明列出受影响的约束和对应验证证据；具体格式遵循相关验收卡。

## 验证要求

行为改动按 `docs/refactor/verification.md` 的受影响验收卡验证；没有卡片要求的真机证据时不得报告验收通过。检查口径和具体命令见验收卡与 `docs/guides/build-and-deploy.md`。

一个功能一个提交；不要将构建产物、日志、缓存和临时文件加入源码变更。

## `.tmp/` 临时文件规则

- 每个任务先创建独立目录，例如 `.tmp/<task-name>/`；禁止把日志、截图、反汇编、探针输出直接散落在 `.tmp/` 根目录。
- 只允许保存本次任务可重建的中间文件、诊断日志、截图和临时输入；源码、可复用脚本、第三方工具、APK 交付物和长期证据不得写入 `.tmp/`。
- 不得删除 `apk/`、`output/` 或 `archive/` 等内容。
- `.tmp/` 已被 Git 忽略，不得使用强制添加将其纳入提交。
