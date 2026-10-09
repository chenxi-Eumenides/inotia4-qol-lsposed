# 脚本目录规则

- 根级入口：`build-debug.sh`（日常 Debug 构建）、`build-release.sh`（仅按仓库 Release 规则）、`patch-apk.sh`（生成集成 APK）。
- `analysis/parse_qol_save.py`：解析 `.qol_save` 备份；`analysis/frida/save0-repair-patch.js`：已登记的可复用存档修复 patch；一次性逆向探针不保留在当前项目。
- `verification/`：smoke、性能、回归和 live session。
- `device/`：adb、设备启动、安装和触摸自动化。
- `data/`：静态数据导出、校验和 assets 打包。
- `maintenance/`：符号、依赖和工作区检查。
- `web/build_map_arranger.py`：从静态地图数据生成 `tools/web/map-arranger.html`。
- `keys/`：`patch-apk.sh` 默认签名密钥（AOSP 公开 testkey，BKS，非机密）；生成与校验步骤见 `docs/guides/build-and-deploy.md` §3.1。
- 一次性实验脚本放入项目根目录 `archive/experiments/`，不得混入正式入口。
- 脚本输出不得写入脚本源目录；输出位置遵循根目录代理规则。
- 例外：**生成型文档**由对应脚本直接写入受版本控制的当前文档目录（当前白名单：`scripts/maintenance/gen_baseline_doc.py` → `docs/refactor/baseline.md`、`scripts/maintenance/gen_map_doc.py` → `docs/refactor/map.md`），属生成物而非「工具输出」。归档目录不得作为生成脚本的输出目标。
- 日志、截图和中间文件不得写入脚本目录。
- Android 构建入口和 Python 运行环境遵循根目录代理规则及构建指南。
