# 开发环境与工具链

> 日期：2026-08-05 ｜ 主机：Manjaro Linux x86_64，内核 6.1.174
> 本文档承接 README「快速了解」之外的详细环境信息：依赖清单、SDK/NDK 路径、Python 环境、关键命令、环境验证记录。

## 1. 依赖清单总览（2026-08-05 全部就绪）

> 下表为开发机既有系统级安装（yay 方式）。按「目录规范与环境隔离」，核心要求是**工具输出的文件必须落在项目文件夹内**。
> **SDK 路径：`/opt/android-sdk/`（platforms/android-34 + build-tools/37.0.0 + platform-tools）**

### A. 基础运行环境 ✅

| 程序 | 用途 | 版本 |
|---|---|---|
| OpenJDK 17 | 全部 Java 工具依赖（已切换为默认） | 17.0.19 |

### B. 静态分析链 ✅

| 程序 | 用途 | 版本 |
|---|---|---|
| apktool | 解码 APK / 提取静态数据 | 3.0.3 |
| jadx | 反编译定位 hook 点 | 1.5.6 |
| apksigner / zipalign / aapt2 / d8 | 签名/对齐/资源编译/dex 转换 | build-tools 37.0.0 |
| uber-apk-signer | 一键签名+对齐+验证 | 1.3.0 |

### C. 模块开发链 ✅（全部就绪）

| 程序 | 用途 | 状态 |
|---|---|---|
| Gradle | 命令行构建模块 APK | ✅ **8.11.1（唯一版本，v0.4.30 固定）**：wrapper 发行版缓存于项目 `.gradle/`；zip 本地备份 `tools/gradle-8.11.1-bin.zip`（wrapper.properties distributionUrl 指向本地 file://，网络抖动不重下载）。⚠️ 系统 gradle 9.6.1 与 AGP 8.7.3+kapt 1.9.24 不兼容（kaptDebugKotlin 创建失败），不可用 |
| Android SDK platform | 提供 android.jar（`/opt/android-sdk/platforms/android-34/`） | ✅ android-34 |
| Android SDK build-tools | 编译/打包 Android 模块 | ✅ 37.0.0 |
| **Android NDK** | 编译 native 数据访问层 | ✅ **r26d（26.3.11579264）**，**项目内 `tools/ndk/`**（瘦身至 2.0G，仅 ARM ABI） |
| libxposed API（compileOnly） | LSPosed 现代 Xposed API（`io.github.libxposed:api:101.0.1`） | 📦 Gradle 依赖（项目内） |
| AndServer 库 | 进程内 HTTP 服务 | 📦 Gradle 依赖（项目内） |
| LSPatch (lspatch.jar) | 集成免 root 版 APK | ✅ `tools/lspatch/lspatch.jar`（v0.6，10.8MB） |

### D. 部署与验证链 ✅

| 程序 | 用途 | 状态 |
|---|---|---|
| LSPosed 框架 | 模块运行框架（手机端） | ✅ 用户已有 |
| adb | 装 APK/抓日志/端口转发 | ✅ 1.0.41（android-tools 36.0.1） |
| python-frida（系统级） | 快速原型验证 hook 点 | ✅ 17.7.2（系统 pacman 安装，CLI 未装） |
| **项目 venv（uv 管理）** | 项目内 Python 依赖（frida 17.16.4 + frida-tools 14.10.4） | ✅ `.venv/`，见下文「Python 环境」 |

### E. 关于 MCP 工具套件

- MCP 套件（如 zinja-coder 系列）是**操作接口层**，非工具本身，底层程序仍需自行安装
- 本方案核心工作（模块开发、LSPosed 部署）在 MCP 套件覆盖范围之外
- **结论：无需安装 MCP 工具套件**，仅需 jadx CLI 做静态分析

## 2. Python 环境（uv 管理）

- 项目级 venv：`.venv/`（uv 创建，Python 3.13.11）
- 依赖清单：`pyproject.toml` + 锁定文件 `uv.lock`
- 已装依赖：**frida 17.16.4 + frida-tools 14.10.4**（含 CLI：frida-ps/frida-trace 等）
- 用法：`uv run <命令>`（如 `uv run frida-ps -U`、`uv run python script.py`）
- 版本约束：frida 库版本须与目标设备上的 **frida-server 版本匹配**（当前 17.16.4，后续部署设备时确认）
- 说明：系统级 pacman 的 python-frida 17.7.2 保留但**不用于本项目**；项目一律用 `.venv/`
- 镜像：pyproject.toml 已配置阿里云 PyPI 镜像（`mirrors.aliyun.com`）

## 3. 关键命令

### 3.1 真机开发循环（命令速查）

> 本节仅列核心命令速查。

```bash
# ① 构建模块（workdir: module/，缓存落项目 .gradle/）
# wrapper zip 曾被清理，直接用缓存发行版（或先让 wrapper 补下载）
# ⚠️ zsh 下 `*/` 通配符不展开会报错，必须写完整路径（目录名 bpt9gzteqjrbo1mjrsomdt32c 固定）
GRADLE_BIN=$PWD/../.gradle/wrapper/dists/gradle-8.11.1-bin/bpt9gzteqjrbo1mjrsomdt32c/gradle-8.11.1/bin/gradle
GRADLE_USER_HOME=$PWD/../.gradle $GRADLE_BIN :app:assembleDebug --no-daemon
# 产物 → output/inotia4-qol-lsposed-<版本>.apk（复制 + 版本号递增，见 README 规则 6）
# 命名格式固定：inotia4-qol-lsposed-vX.Y.Z.apk（如 v0.4.56）

# ② 部署（覆盖安装，LSPosed 启用状态按包名保留）
# 默认操作真机2（192.168.3.54）；若同时连着真机1 需加 -s <序列号> 区分。
# 当前构建命令的实际产物是 app/build/outputs/apk/debug/app-debug.apk。
adb -s 192.168.3.54:5555 install -r app/build/outputs/apk/debug/app-debug.apk

# ③ 重启游戏（让 Xposed 重新注入，模块更新生效的必需步骤）
# 按包名 force-stop 即可，**无需 pid**；monkey 启动与桌面点击等价
# 游戏启动约 15-18 秒到主菜单（state=4）；重启后首屏若为通知栏（NotificationShade）先 input keyevent 4 关闭
adb -s 192.168.3.54:5555 shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common
adb -s 192.168.3.54:5555 shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1

# ③a 真机2启动弹窗前置（2026-08-27 实测必需；仅此一步使用触摸脚本）
# 脚本通过 ANDROID_SERIAL 固定目标真机2，坐标不适用于其他设备。
ANDROID_SERIAL=192.168.3.54:5555 uv run python scripts/touch_automation.py --inject input click 420,280 1.0

# ④ 等待 API 就绪（8088 端口；curl 轮询比 /proc/net/tcp 可靠）
# API 可达（能返回 JSON）即代表模块已注入、游戏启动完成；轮询到 "screen" 字段说明模块数据通路就绪
until curl -s -m 2 http://192.168.3.54:8088/api/health | grep -q '"ok"'; do sleep 2; done

# ⑤ 进入游戏世界（推荐：API enter_slot；触摸方案已弃用）
curl -s -X POST http://192.168.3.54:8088/api/system/enter_slot -H "Content-Type: application/json" -d '{"slot":0}'
# 验证：screen=world 即进入世界
curl -s http://192.168.3.54:8088/api/ui/screen
```

> **游戏重启与进程定位**：`am force-stop <包名>` 按包名杀进程，**不需要 pid**（pid 每次重启都变，不必查询）。
> frida attach 也用**进程显示名**（`adb shell ps | grep 包名` 的 NAME 列，如 "Inotia4"），不用 pid。
> 仅当需要 pid 时：`adb shell pidof com.com2us.inotia4.normal.freefull.google.global.android.common`。

### 3.2 常用脚本速查（均须 `uv run`）

| 脚本 | 用途 | 用法 | 默认 |
|---|---|---|---|
| `scripts/analyze/check_symbols.py` | 符号一致性校验（**改 game_symbols.h 后必跑**） | `uv run python scripts/analyze/check_symbols.py [libgame.so路径]` | `apk/decoded/lib/arm64-v8a/libgame.so`，比对 120+ 符号；**新增符号须登记 `SYMBOL_TO_MACRO` 映射** |
| `scripts/analyze/api_poll.py` | 连续轮询 player/party/inventory 检测字段变化 | `uv run python scripts/analyze/api_poll.py <IP> [间隔秒] [次数]` | `192.168.3.54`, 2.0s, 30 次 |
| `scripts/analyze/live_session.py` | 联调全自动会话（局域网/Tailscale 通用采样） | `uv run python scripts/analyze/live_session.py [IP] [时长上限分钟]` | `192.168.3.54`, 上限 5min |
| `scripts/parse/package_assets.py` | 静态数据重打包进模块 assets（M3 产物 → module/assets） | `uv run python scripts/parse/package_assets.py` | 28 表 + zh-Hans/en 语言 |
| `scripts/touch_automation.py` | adb 触摸注入（执行模式）+ 实时检测（无参数=检测模式） | `uv run python scripts/touch_automation.py click 100,200 0.5 ...` | 3168x1440 逻辑坐标，自动旋转校准 |

### 3.3 设备连接方式（两台真机）

> **项目有两台真机，不是同一台手机**（2026-08-12 确认）：

| 设备 | 局域网 IP | Tailscale IP | 说明 |
|---|---|---|---|
| **真机1** | `192.168.3.11:5555` | `100.110.139.83:5555` | 原主力机（OnePlus 13，root + Zygisk-LSPosed），**UI 点击坐标文档（ui-click-coordinates.md）所有坐标均针对此机**（3168x1440 窗口坐标系） |
| **真机2** | `192.168.3.54:5555` | 无（未配置） | 第二台真机（当前主力，2026-08-12 起），**完全用 API 操控，不适用触摸坐标** |

连接方式（按优先级依次尝试）：

1. **USB**：`adb devices`
2. **局域网**：`adb connect <设备IP>:5555`（真机1=`192.168.3.11`，真机2=`192.168.3.54`）
3. **Tailscale**：`adb connect 100.110.139.83:5555`（仅真机1）

> **重要**：两台设备分别 `adb connect` 后由 `adb -s <序列号> <命令>` 区分；`adb` 默认连最后连接的设备。日常默认以**真机2（192.168.3.54）**为开发机，命令速查中的 IP 均指真机2。
> **UI 坐标限制**：真机2仅允许在启动弹窗前置使用已验证坐标 `(420,280)`；其余进入存档、背包读取、移动、保存和验收全部使用 HTTP API。真机1坐标仍不在本项目当前验收范围内。

### 3.4 其他常用命令

```bash
# 符号查询（workdir: 项目根，libgame.so 符号表）
grep " INVEN_GetMoney" apk/decompiled/libgame-symbols.txt

# 抓模块与扩展背包 native 日志（文件日志在手机 sdcard/Android/data/<游戏包>/files/）
adb logcat -s Inotia4Export:V Inotia4VirtBag:V

# 反汇编定位（改 game_symbols.h 时用）
tools/ndk/.../llvm-objdump -d --start-address=0x... --stop-address=0x... apk/decoded/lib/arm64-v8a/libgame.so
```

> 构建注意：Gradle 中间产物在 `module/**/build/`，最终 APK 复制到 `output/` 后验收交付；
> `GRADLE_USER_HOME=$PWD/.gradle` 为可选构建缓存隔离（非强制）。

## 4. 环境验证记录（2026-08-05）

| 验证项 | 结果 |
|---|---|
| apktool | ✅ 3.0.3 |
| jadx | ✅ 1.5.6 |
| apksigner | ✅ 0.9（build-tools 37.0.0） |
| adb | ✅ 1.0.41 |
| zipalign | ✅ |
| uber-apk-signer | ✅ 1.3.0 |
| Java | ✅ OpenJDK 17.0.19（已切换默认，原 1.8 弃用） |
| python-frida | ✅ 17.7.2（CLI 未装，可选） |
| Android SDK | ✅ `/opt/android-sdk/`：platforms/android-34 + build-tools/37.0.0 + platform-tools |
| LSPatch | ✅ `tools/lspatch/lspatch.jar`（v0.6，10.8MB，`java -jar` 可运行） |
| APK 解码 | ✅ `apktool d` 成功，输出至 `apk/decoded/` |

## 5. 环境相关已知待办

> 环境/部署相关待办已统一收录至 `docs/backlog.md`（部署/环境表），本节不再维护。

已完结（历史记录）：
- [x] **y7000 模拟器环境**（2026-08-05 实测完结：TCG ARM VM boot 25+ 分钟未完成；x86_64 转译路线 frida 不可用 + LSPatch native 高风险）→ 模拟器路线冻结，转向真机
- [x] **实体 root 手机就绪**（✅ oneplus-13 已配置 root + Zygisk-LSPosed 并真机联调）
- [x] **android.jar 引用方式**（已用 `local.properties` 的 `sdk.dir` 解决）

## 5a. 环境踩坑记录（历次会话沉淀）

> 环境/工具链相关的踩坑集中在本文档；操作端点逆向结论已归档（原 `docs/research/` 于 2026-08-16 清理）。

1. **sdkmanager 旧版 JDK 不兼容**（javax.xml.bind 缺失）→ 直接写 license 文件绕过（不跑 sdkmanager --licenses）。
2. **AndServer 坐标**：2.x 是 `com.yanzhenjie.andserver:api/annotation/processor`（+kapt），不是 `com.yanzhenjie:andserver`。
3. **AndServer 2.1.12 插件无 Gradle marker**（`com.yanzhenjie.andserver.gradle.plugin` 在 Maven Central 404）→ 必须 `buildscript { classpath("com.yanzhenjie.andserver:plugin:2.1.12") }` + `apply(plugin=...)`，不能用 `plugins {}` DSL。且 **2.1.12 就是最新版**（勿升级）。
4. **AndServer processor 依赖缺下载**：`commons-collections4`/`commons-lang3` 等首次解析未下载 → 先跑 `:app:dependencies --configuration kaptDebug` 触发下载。
5. **SDK 无 CMake**：NDK 瘦身移除 cmake。系统 cmake 4.4 通过 `local.properties` 加 `cmake.dir=/usr` 使用（AGP 找 `<dir>/bin/cmake`）；`android.ndkVersion` 须显式声明（26.3.11579264 匹配 r26d）。
6. **libxposed 101 写法**：`class XposedMain : XposedModule()` + `override fun onModuleLoaded(param: XposedModuleInterface.ModuleLoadedParam)`（101 起无参构造 + attachFramework 自动调用；参考 LSPosed/CorePatch）。
7. **y7000 跨平台工具链**（2026-08-05 实测）：Windows OpenSSH 结束会话会终止 Start-Process 后台进程（长任务用 schtasks Interactive 登录）；aria2 `--all-proxy` 不支持 socks5://（只认 http://，127.0.0.1:20170 多线程 GB 级/分钟）；wsl.exe 输出为 UTF-16（PowerShell 调用后 grep 判二进制 → 重定向文件再 Get-Content）。
8. **Gradle wrapper zip 曾被清理**：wrapper 需重新下载（services.gradle.org 超时）→ 直接用缓存发行版 `../.gradle/wrapper/dists/gradle-8.11.1-bin/*/gradle-8.11.1/bin/gradle`。
9. **AGP 依赖下载慢**（国外仓库）→ 阿里云镜像（settings.gradle.kts 已配）。
10. **zsh 通配符不展开**（2026-08-12 实测）：`GRADLE_BIN=$PWD/../.gradle/wrapper/dists/gradle-8.11.1-bin/*/...` 中 `*/` 在 zsh 下**不展开**直接报 `没有那个文件或目录` → 写完整路径 `.../bpt9gzteqjrbo1mjrsomdt32c/gradle-8.11.1/bin/gradle`。
11. **frida-server 重启后需 su 启动**（2026-08-12 实测）：设备重启后 `/data/local/tmp/frida-server` 需 `adb shell su -c 'nohup /data/local/tmp/frida-server >/dev/null 2>&1 &'`（root + nohup），普通 `adb shell "frida-server &"` 无权限启动失败。
12. **通知栏遮挡启动**（2026-08-12 实测）：设备重启后首屏可能是 NotificationShade（`dumpsys window` mCurrentFocus 显示），monkey 启动游戏前先 `input keyevent 4` 关闭通知栏回到桌面，否则游戏未真正启动（8088 无监听）。
13. **两台真机**（2026-08-12 确认）：真机1=`192.168.3.11`（局域网）+`100.110.139.83`（Tailscale，同一台）；真机2=`192.168.3.54`（另一台，当前主力）。真机2仅启动弹窗前置例外使用 `(420,280)`，其余完全用 API 操控。详见 §3.3。
14. **真机2启动弹窗**（2026-08-27 实测）：monkey 启动后可能出现无 API 跳过的弹窗；启动流程必须追加 `ANDROID_SERIAL=192.168.3.54:5555 uv run python scripts/touch_automation.py --inject input click 420,280 1.0`，点击后再轮询 `/api/health` 和 `/api/ui/screen`。除该弹窗前置外，不使用真机2触摸坐标。

## 6. 关联文档

- 项目总览 / 目录规范：`README.md`
- 代码结构（NDK/CMake/依赖配置说明）：`architecture.md`
- 开发待办：`docs/backlog.md`
