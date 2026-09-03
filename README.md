# 艾诺迪亚4 QOL 改动 LSPosed 模块

艾诺迪亚4（Inotia 4）的 LSPosed 模块，提供多项 QOL 改动，并将游戏运行时数据与操作通过 HTTP API 导出，供用户或 AI 进行外部操控。

## 项目目标

- 导出游戏数据：运行时状态（血量、金币、等级、坐标、背包装备、技能、佣兵等）+ 静态资源（数值表、道具配置、地图数据等，全量提取），供外部程序读取
- 导出操作能力：游戏内操作（移动、使用道具、装备、队伍等）通过 API 触发，供外部程序操控
- 通过 HTTP API（REST + JSON）对外提供，局域网内可访问
- 游戏本体零修改：模块独立于游戏 APK，以 Xposed 模块形式注入
- 双交付物，模块版为主：日常构建 LSPosed 模块版；LSPatch 集成版（免 root）仅在需要时构建
- 待确认：API 消费者数量与访问方式（单程序 / 多客户端）

## 项目背景

- **游戏**：艾诺迪亚4（Inotia 4，Com2uS）盗版大修 20260704，单机/离线，无源码只有 APK（48.4MB），无加固，已被重打包重签。包名：`com.com2us.inotia4.normal.freefull.google.global.android.common`（adb/LSPosed scope/frida 等开发命令均需此包名）
- **引擎**：原生 Java（classes.dex + classes2.dex）+ 自研引擎 native 库 `libgame.so`（arm64-v8a + armeabi-v7a，**未 strip 符号**）
- **部署目标**：实体 root 手机（LSPosed 模块版）为主路线；Waydroid 集成版受游戏 ARM-only 限制，降级为延伸目标
- 开发环境与工具链见 `docs/guides/build-and-deploy.md`

## 硬性要求

1. **先读文档再开发**：开始任何开发/操作前，先按下方「文档地图」定位所需文档并通读；禁止重复探索、重复尝试文档已记载的结论与踩坑经验
2. **先读文档再提问**：有困惑或疑问时，先查「文档地图」对应文档自行确认；仍不确定时，再问用户

## 技术方案（概要）

- **一套代码，双交付物（模块版为主）**：导出模块只开发一次，日常构建 LSPosed 模块版；LSPatch 集成版仅在需要时集成
  - **形态 A（模块版）✅ 主路线**：LSPosed 独立模块 APK（libxposed 101 + AndServer 内嵌 HTTP 服务），游戏 APK 零修改，日常开发/构建对象
  - **形态 B（集成版）🔄 按需集成**：LSPatch 集成 APK（免 root 单文件），受游戏 ARM-only 限制，仅在需要免 root 部署时构建
- **数据访问（✅ 真机验证）**：模块 native 层通过 **`/proc/self/maps` 基址 + ELF 符号动态解析**读 libgame.so 全局变量与 Getter 函数（**不用 dlopen/dlsym**——namespace 隔离会加载独立副本读不到数据）。v0.5.15 起：204 个符号走 `.dynsym` 符号名动态解析、42 个 GOT 槽走 RELATIVE addend 反查（symbol_registry.h 单一来源 + symbol_resolver），VMA 仅兜底——**换游戏版本零操作，地址漂移自动适配**
- **API 服务 = Java（AndServer 内嵌于模块进程）**：数据获取在 native（C/C++），API 服务用 Java，两者通过 JNI 桥接，同一进程内运行
- **静态数据**：Python 脚本解析 game_res 格式（M3 完成）→ JSON 数据库（可交付）
- **Frida**：仅开发期原型验证用，不进交付物

> **详细架构与规范见 `docs/development/architecture.md`（唯一权威）**；
> API 端点与数据模型见 `docs/reference/api-reference.md`。

## 当前状态

- 开发待办与完成标准见 `docs/development/planning/backlog.md`（唯一待办来源）

## 交付物

| 交付物 | 说明 | 部署目标 | 状态 |
|---|---|---|---|
| **导出模块 APK** | Xposed 模块，Hook 游戏 + 提供 REST API | 手机（LSPosed） | ✅ 最新版本见 `output/` |
| **集成版 APK（modded.apk）** | LSPatch 集成模块+游戏，免 root 单文件 | 按需集成（免 root 部署时） | 🔄 已构建待验证 |
| **静态数据 JSON 数据库** | 解析的数值表/配置/资源 | 两者共用 | ✅ 完成（`apk/static-data/json/`，22MB） |

## 项目文件结构

```
projects/inotia4-qol-lsposed/
├── apk/                                    # 【输入物】原始 APK + 解码/反编译中间产物 + 解析出的静态库（apk/static-data/，可再生成）
├── tools/                                  # 【工具】第三方工具（LSPatch/NDK 等，项目内隔离）
├── scripts/                                # 【工作区】开发、分析、数据和验证脚本（analysis/verification/device/data/maintenance）
├── module/                                 # 【交付·源码】Xposed 导出模块 Gradle 工程
├── output/                                 # 【交付·二进制】构建产物 APK（最新版本见「交付物」表）
├── docs/                                   # 【交付·文档】按用途分为 guides/development/reference/history
├── log/                                    # 运行日志
├── archive/                                # 【归档】探索研究中间产物（frida 探查脚本/反汇编/截图等，不入库）
├── .gradle/                                # 可选构建缓存隔离
└── .tmp/                                   # 按任务隔离的临时文件（任务完成后清理）
```

> 目录明细与构建产物见 `docs/guides/build-and-deploy.md`。

## 文档地图（文档结构）

> **分级阅读策略**：第一级文档无论什么任务都必读；第二级文档任务涉及该领域即必读；第三级文档仅任务涉及具体内容才读。
> 不确定信息归属时，先查下表定位到对应文档再动手。

| 文档 | 主题 | 分级 | 权威性 |
|---|---|---|---|
| **本文件 README.md** | 项目总览、目标、交付物、目录规范 | **第一级** | 总览 |
| **docs/development/architecture.md** | 模块代码结构和规范 | 第一级 | **代码唯一权威** |
| **docs/reference/api-reference.md** | REST API 规格 | 第二级 | API 权威 |
| **docs/guides/game-guide.md** | 游戏与模块使用手册 | 第二级 | 使用指南 |
| **docs/guides/build-and-deploy.md** | 构建、设备和部署 | 第二级 | 操作指南 |
| **docs/development/planning/backlog.md** | 当前开发待办 | 第二级 | 待办权威 |
| **docs/development/features/extension-bag/control-plane.md** | 扩展背包开发与验收 | 第二级 | 功能权威 |
| **docs/INDEX.md** | 文档分类和阅读入口 | 第一级 | 文档导航 |

> 职责划分原则：**每个文档只有一个主题，互不重复**；结构/规范以 `docs/development/architecture.md` 为准，API 以 `docs/reference/api-reference.md` 为准，当前待办以 `docs/development/planning/backlog.md` 为准。

## 目录规范与环境隔离（强制）

> **原则**：本项目的所有文件、中间产物、临时文件、构建产物与最终交付物，一律存放于本目录
> `projects/inotia4-qol-lsposed/` 内。**工具的输出文件（解码产物、反编译输出、构建产物、生成的 APK、
> 解析结果、日志、截图等）必须落在项目文件夹内**，禁止写到项目文件夹之外。

1. **工具输出（必须项目内）**：凡工具会产出文件——解码产物（apktool）、反编译输出（jadx）、构建产物（Gradle/Android 构建）、生成的 APK、解析出的 JSON、日志与截图——输出路径必须落在项目文件夹内（`apk/decoded/`、`output/`、`apk/static-data/`、`.tmp/` 等），禁止散落到系统目录或项目外 `/tmp`
2. **Python**：项目依赖一律用项目内 `.venv/`（`uv run`），禁止向系统 Python 安装项目依赖；系统 pacman 的 python-frida 不用于本项目
3. **构建与交付**：必须使用 `scripts/build-debug.sh` 或 `scripts/build-release.sh`；Gradle 中间产物在 `module/**/build/`，最终 APK 由脚本复制到 `output/` 后验收交付。
4. **临时文件**：统一写入 `.tmp/<task-name>/`，只保存可重建的日志、截图、反汇编和一次性中间文件；任务结束后清理对应目录。当前仅保留可重复验收、数据处理、设备操作、符号检查脚本和已登记的 Frida patch，一次性逆向探针不进入正式脚本目录。
5. **只读环境依赖**：Android SDK（`/opt/android-sdk`）属运行环境，仅引用，项目文件不写入其中
6. **版本递增**: 只有在一个或多个功能完成后，才能更新版本号，每次只更新0.0.1。只有用户明确，才升小版本号0.1.0。
7. **版本提交（强制）**：每次递增版本号并成功构建出一个新版本 APK 后，必须将代码变更提交到 git；提交信息注明版本号与变更摘要（如 `feat(v0.2.21): 新增 xxx`）。新版本只有代码已提交后才算完成

## 协作约定

- **sudo 由用户执行**（一次性命令模式；用户拒绝长期免密 sudoers）
- **大文件下载可委托用户**（模拟器/工具镜像如 QEMU 用户自装），子代理下载大文件可委托
