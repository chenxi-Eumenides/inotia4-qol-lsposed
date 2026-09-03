# 脚本目录规则

- `analysis/frida/save0-repair-patch.js`：已登记的可复用存档修复 patch；一次性逆向探针不保留在当前项目。
- `verification/`：smoke、性能、回归和 live session。
- `device/`：adb、设备启动、安装和触摸自动化。
- `data/`：静态数据导出、校验和 assets 打包。
- `maintenance/`：符号、依赖和工作区检查。
- 一次性实验脚本放入项目根目录 `archive/experiments/`，不得混入正式入口。
- 脚本输出只能写入 `apk/`、`output/`、`archive/` 或 `.tmp/`。
- `.tmp/` 输出必须放入任务专属子目录，任务结束后清理；脚本不得把日志、截图或中间文件写入脚本目录根部。
- Android 构建脚本是唯一构建入口：使用 `scripts/build-debug.sh` 或 `scripts/build-release.sh`，不要在其他脚本或文档中调用 `gradle`/`./gradlew`。
- 脚本必须使用项目 `.venv/`，通过 `uv run` 执行 Python 依赖。
