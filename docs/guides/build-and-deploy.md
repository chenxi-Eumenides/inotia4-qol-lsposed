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
| Gradle | 命令行构建模块 APK | ✅ **8.11.1（唯一版本，v0.4.30 固定）**：完整发行版缓存于项目隐藏目录 `.gradle/wrapper/dists/`，统一通过 `scripts/build-debug.sh` 自动发现和调用；不要求 `tools/` 保存 zip。⚠️ 系统 Gradle 版本不作为本项目构建入口 |
| Android SDK platform | 提供 android.jar（`/opt/android-sdk/platforms/android-34/`） | ✅ android-34 |
| Android SDK build-tools | 编译/打包 Android 模块 | ✅ 37.0.0 |
| **Android NDK** | 编译 native 数据访问层 | ✅ **r26d（26.3.11579264）**，**项目内 `tools/ndk/`**（瘦身至 2.0G，仅 ARM ABI） |
| libxposed API（compileOnly） | LSPosed 现代 Xposed API（`io.github.libxposed:api:101.0.1`） | 📦 Gradle 依赖（项目内） |
| AndServer 库 | 进程内 HTTP 服务 | 📦 Gradle 依赖（项目内） |
| **NPatch（默认集成工具）** | 集成免 root 版 APK | ✅ `tools/lspatch/npatch-v1.0.7-741-release.jar`（v1.0.7）；LSPatch v0.6 / v1.2 保留备用，可 `--lspatch-jar` 指定 |

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
# ① 构建模块（workdir：项目根目录；脚本自动发现隐藏目录 .gradle/ 中的 Gradle 8.11.1）
# 禁止直接执行 ./gradlew、系统 gradle 或手写缓存路径。
scripts/build-debug.sh
# 默认使用项目缓存离线构建；如需传递额外 Gradle 参数，直接追加参数即可：
scripts/build-debug.sh --offline
# 正式版构建并复制为 output/inotia4-qol-lsposed-v<version>-release-unsigned.apk
scripts/build-release.sh
# Debug 产物 → output/inotia4-qol-lsposed-debug-<YYMMDDHHMM>-<sha256前12位>.apk；脚本只保留最新 3 份 Debug APK
# Release 产物 → output/inotia4-qol-lsposed-v<version>-release-unsigned.apk；版本号来自 build.gradle.kts，脚本只保留最新 2 份 Release APK
# 如需强制离线，可追加 Gradle 参数：scripts/build-release.sh --offline
# 命名格式固定：inotia4-qol-lsposed-vX.Y.Z.apk（如 v0.4.56）
# 多目标包名：逗号分隔，同时写入 LSPosed scope.list 和模块运行时过滤。
scripts/build-release.sh -PtargetPackages=com.com2us.inotia4.normal.freefull.google.global.android.common,com.com2us.inotia4.qol.patched

# ⑥ 按需生成集成版（原始游戏 APK + 本模块 APK → 单独 APK）
# 默认使用 NPatch（tools/lspatch/npatch-v1.0.7-741-release.jar，JAR 自带 BouncyCastle，脚本自动注册 BKS provider）。
scripts/patch-apk.sh <原始游戏.apk> <模块.apk>
# 默认输出：output/<原始文件名>-npatched.apk（LSPatch JAR 则为 -lspatched.apk）；已有文件需显式 --force 覆盖。
# NPatch 生成前会自动删除 output/ 下旧 NPatch 产物（*npatch*.apk，不含本次输出），保持输出目录干净。
# 使用 LSPatch（v0.6 / v1.2）：
scripts/patch-apk.sh --lspatch-jar /path/to/lspatch-v1.2-release.jar \
    <原始游戏.apk> <模块.apk>
# NPatch 支持修改输出 applicationId；模块构建时需把新包名加入 targetPackages：
scripts/patch-apk.sh --newpackage com.com2us.inotia4.qol.patched <原始游戏.apk> <模块.apk>
# signature bypass 按需显式设置；不要未经验证启用 level 3。
scripts/patch-apk.sh --sigbypasslv 2 <原始游戏.apk> <模块.apk>

# ② 部署（覆盖安装，LSPosed 启用状态按包名保留）
# 默认操作单台真机（<设备IP>）；多设备时用 -s <序列号> 区分。
# 当前构建命令的实际产物是 app/build/outputs/apk/debug/app-debug.apk。
adb -s <设备序列号> install -r app/build/outputs/apk/debug/app-debug.apk

# ③ 重启游戏（让 Xposed 重新注入，模块更新生效的必需步骤）
# 按包名 force-stop 即可，**无需 pid**；monkey 启动与桌面点击等价
# 游戏启动约 15-18 秒到主菜单（state=4）；重启后首屏若为通知栏（NotificationShade）先 input keyevent 4 关闭
adb -s <设备序列号> shell am force-stop com.com2us.inotia4.normal.freefull.google.global.android.common
adb -s <设备序列号> shell monkey -p com.com2us.inotia4.normal.freefull.google.global.android.common -c android.intent.category.LAUNCHER 1

# ③a 真机启动弹窗前置（2026-08-27 实测必需；仅此一步使用触摸脚本）
# 脚本通过 ANDROID_SERIAL 固定目标真机，坐标不适用于其他设备。
ANDROID_SERIAL=<设备序列号> uv run python scripts/device/touch_automation.py --inject input click <启动弹窗坐标> 1.0

# ④ 等待 API 就绪（8088 端口；curl 轮询比 /proc/net/tcp 可靠）
# API 可达（能返回 JSON）即代表模块已注入、游戏启动完成；轮询到 "screen" 字段说明模块数据通路就绪
until curl -s -m 2 http://<设备IP>:8088/api/health | grep -q '"ok"'; do sleep 2; done

# ⑤ 进入游戏世界（推荐：API enter_slot；触摸方案已弃用）
curl -s -X POST http://<设备IP>:8088/api/system/enter_slot -H "Content-Type: application/json" -d '{"slot":0}'
# 验证：screen=world 即进入世界
curl -s http://<设备IP>:8088/api/ui/screen
```

> `targetPackages` 默认只有原版游戏包名。需要让同一个 LSPosed 模块覆盖多个包时，使用
> `-PtargetPackages=pkg.one,pkg.two`；构建会生成多行 `META-INF/xposed/scope.list`，并让运行时只在这些包中初始化。
> 包名必须是合法 Android applicationId。NPatch 的 `--newpackage` 只改输出 APK 的 manifest/applicationId，原游戏 dex 中的类名和资源 ID 不变；因此新包名必须同时加入此参数并重新构建模块，不能只修改 APK 文件名。该游戏的资源表仍保留原资源 namespace，模块已在改包名进程中兼容 `CResource.R()` 和 `Resources.getIdentifier()`。

> 当前默认 Release 配置已同时包含原包和 `com.com2us.inotia4.qol.patched`；不传 `-PtargetPackages` 即可生成支持两个包的模块 APK。只有新增其他目标包时才需要通过 Gradle 属性覆盖列表，`patch-apk.sh` 不会自动重建模块。

> 该游戏的原 Manifest 声明了 `C2D_MESSAGE` 自定义权限。独立包名输出会在 NPatch 完成后自动移除这项冲突声明，并使用 NPatch 内置证书重新签名；不要手工修改 NPatch 输出 APK，否则会破坏 APK 签名。`--newpackage` 流程仍保留 NPatch 默认的原 APK 签名绕过阶段，只有 Manifest 后处理阶段才执行重签名。

### 3.2 Release 流程（仅用户明确要求时执行）

> 日常开发与验证只允许 `scripts/build-debug.sh`，且提交不得变更版本号。只有用户明确要求
> release 时才执行本节流程。

1. **变更版本号**：`module/app/build.gradle.kts` 的 `versionName` `+0.0.1`（`versionCode` 同步 +1）；
   仅用户明确才升小版本 `0.1.0`。
2. **构建模块 Release APK**：`scripts/build-release.sh`，产物
   `output/inotia4-qol-lsposed-v<version>-release-unsigned.apk`。
3. **生成 3 个 NPatch 集成版**（`scripts/patch-apk.sh <游戏.apk> <模块.apk>`，输入在 `apk/game-apk/`）：
   - 原版：`艾诺迪亚4_v1.3.2_原版.apk` → 发布名 `inotia4-qol-original-npatched.apk`
   - 大修版：`艾诺迪亚4_v1.3.2_盗版大修_<日期>.apk` → 发布名 `inotia4-qol-overhaul-<日期>-npatched.apk`
   - monster 版：`Inotia4_v<游戏版本>_monster_<版本>.apk` → 发布名 `Inotia4_v<游戏版本>_monster_<版本>-npatched.apk`
4. **推送 GitHub**：`git push github`。
5. **发布 Release**：`gh release create v<version> <4 个 APK> --title v<version> --notes-file <说明>`；
   说明覆盖「上一个版本 → 当前版本」的全部改动（新增 / 优化 / 修复 / 发布文件 / 致谢）。
6. **附件命名（强制）**：GitHub CLI 上传的附件名必须全 ASCII、不得含中文；先核对上一次发布
   （`gh release view <上一个 tag> --json assets`）的命名再上传。
> 体积说明（2026-09-04 实测）：独立包名 APK 约 92MB，比普通包名（约 52MB）大 40MB。原因是 NPatch 用 ZIP 重叠条目让内嵌的 `assets/npatch/origin.apk`（46MB 原包副本）与宿主数据共享存储，而删除冲突权限的 unzip/zip 重打包和 apksigner 重签名会把重叠条目物化成两份独立数据。已验证不可行的瘦身路径：预处理权限 + `-l 0` 虽能保住重叠（52MB），但 NPatch 重写 zip 时会把 STORED 资源重压缩为 DEFLATED，游戏引擎 mmap 直读崩溃（`SGL_Texture::FromResource`，Scudo misaligned pointer）；`-l 1` 以上又必须读取未修改原包的原始签名，预处理输入会报 `get original signature failed`。除非 NPatch 上游提供「删除指定权限」或「保留 STORED」选项，92MB 是当前唯一稳定形态。
>
> LSPatch 集成模式会把模块嵌入目标 APK，生成的 APK 不需要 LSPosed 或 LSPatch Manager 常驻；更换模块必须重新 patch。脚本只接受 `output/` 下的输出路径，并在完成后打印 SHA-256。
>
> 上游 JingMatrix/LSPatch 当前最新稳定版为 **v1.2**（2026-08-23），发行页提供 `lspatch-v1.2-487-release.jar`。v1.0 起运行时基于 Vector 并使用 modern libxposed API 102；本项目模块按 API 101 编译，因此脚本支持通过 `--lspatch-jar` 试用新版本，但不自动覆盖项目内 v0.6。正式切换前必须验证模块加载、native 库加载和 API 服务启动。
>
> 下载地址：[LSPatch v1.2 Release](https://github.com/JingMatrix/LSPatch/releases/tag/v1.2)。项目当前目标游戏为 ARM-only，集成 APK 仍需在 ARM64 真机验证。

> **游戏重启与进程定位**：`am force-stop <包名>` 按包名杀进程，**不需要 pid**（pid 每次重启都变，不必查询）。
> frida attach 也用**进程显示名**（`adb shell ps | grep 包名` 的 NAME 列，如 "Inotia4"），不用 pid。
> 仅当需要 pid 时：`adb shell pidof com.com2us.inotia4.normal.freefull.google.global.android.common`。

### 3.2 卡死取证（游戏无响应/疑似死锁）

> 适用症状：画面冻结、输入无响应但进程仍存活（ANR 或原生死锁）；进程已崩溃改看
> tombstone，不在本节范围。扩展背包自死锁（持 `g_virtual_bag_mtx` 期间原版回调重入
> 取锁，见 `rulebook.md` R-44）是本节的典型案例。

1. 确认进程存活并取 pid：

   ```bash
   adb -s <设备序列号> shell pidof com.com2us.inotia4.normal.freefull.google.global.android.common
   ```

   无输出即进程已死，转 tombstone 采集，不走本节后续步骤。

2. 抓全部线程原生栈（真机已 root；输出追加到任务临时目录）：

   ```bash
   mkdir -p .tmp/<task-name>
   adb -s <设备序列号> shell su -c 'debuggerd -b <pid>' > .tmp/<task-name>/backtrace.txt
   ```

3. 定位等待线程：在 `backtrace.txt` 中搜 `std::mutex::lock`、`__pthread_mutex_lock`、
   `futex`；其上层 `libgamebridge.so` 帧即当前取锁点。
   - **同线程自死锁特征**：同一线程栈内同时出现「持锁路径帧」（如
     `remove_item_direct_unlocked`、扩展事务帧）与「重入取锁帧」
     （`std::mutex::lock` ← 库存 Hook wrapper，如 `get_item_count_wrapper`）。
   - **跨线程锁序问题特征**：两个线程各自停在 `futex` 等待，且各持有另一线程需要的锁。
4. 配套 logcat 采集（同一时间窗，用于对齐模块日志锚）：

   ```bash
   adb -s <设备序列号> logcat -d -v time > .tmp/<task-name>/logcat.txt
   ```

   结合 `VIRTBAG_LOG` 锚（`txn committed ...`、`release_cleanup ...` 等）判断停滞点；
   取证后恢复：`adb shell am force-stop <包名>` 并按 §3.1 ③ 重启。
5. 取证文件只写 `.tmp/<task-name>/`，任务结束后清理；结论回填对应验收卡的日志锚，
   不得只写「已卡死」而无线程栈证据。

### 3.3 常用脚本速查（均须 `uv run`）

| 脚本 | 用途 | 用法 | 默认 |
|---|---|---|---|
| `scripts/maintenance/check_symbols.py` | 符号一致性校验（**改 game_symbols.h 后必跑**） | `uv run python scripts/maintenance/check_symbols.py [libgame.so路径]` | `apk/decoded/overhaul/lib/arm64-v8a/libgame.so`，比对 120+ 符号；**新增符号须登记 `SYMBOL_TO_MACRO` 映射** |
| `scripts/verification/api_poll.py` | 连续轮询 player/party/inventory 检测字段变化 | `uv run python scripts/verification/api_poll.py <IP> [间隔秒] [次数]` | `<设备IP>`, 2.0s, 30 次 |
| `scripts/verification/live_session.py` | 联调全自动会话（连续采样） | `uv run python scripts/verification/live_session.py [IP] [时长上限分钟]` | `<设备IP>`, 上限 5min |
| `scripts/data/package_assets.py` | 静态数据重打包进模块 assets（M3 产物 → module/assets） | `uv run python scripts/data/package_assets.py` | 28 表 + zh-Hans/en 语言 |
| `scripts/device/touch_automation.py` | adb 触摸注入（执行模式）+ 实时检测（无参数=检测模式） | `uv run python scripts/device/touch_automation.py click 100,200 0.5 ...` | <逻辑分辨率> 逻辑坐标，自动旋转校准 |

### 3.4 设备连接方式

> 本地真机、网络与触摸坐标等环境信息保存在本地（不纳入仓库）；以下为通用说明。

- 连接：`adb connect <设备IP>:5555`；多设备用 `adb -s <序列号> <命令>` 区分（`adb` 默认连最后连接的设备）。
- 部署与验收在真机上进行；除**启动弹窗前置**外，进档、背包读取、移动、保存和验收全部使用 HTTP API。
- 触摸脚本 `scripts/device/touch_automation.py` 仅用于启动弹窗前置，坐标按本地设备实测填写。

### 3.4 其他常用命令

```bash
# 符号查询（workdir: 项目根，libgame.so 符号表）
grep " INVEN_GetMoney" apk/decompiled/libgame-symbols.txt

# 抓模块与扩展背包 native 日志（文件日志在手机 sdcard/Android/data/<游戏包>/files/）
adb logcat -s Inotia4Export:V Inotia4VirtBag:V

# 反汇编定位（改 game_symbols.h 时用）
tools/ndk/.../llvm-objdump -d --start-address=0x... --stop-address=0x... apk/decoded/overhaul/lib/arm64-v8a/libgame.so
```

> 构建注意：Gradle 中间产物在 `module/**/build/`，最终 APK 复制到 `output/` 后验收交付；
> `GRADLE_USER_HOME=$PWD/.gradle` 为可选构建缓存隔离（非强制）。

### 3.5 临时文件规则

- 每个任务开始前创建独立目录：`.tmp/<task-name>/`；任务名使用小写英文、数字和短横线，避免直接写入 `.tmp/` 根目录。
- `.tmp/<task-name>/` 只保存本次任务可重建的日志、截图、反汇编、探针输出和临时输入；源码、可复用脚本、第三方工具、APK 交付物和长期证据不得写入。
- 任务完成后立即清理对应任务目录；长期需要保留的证据移入 `docs/history/` 或 `archive/`，不得依赖 `.tmp/` 作为长期存储。
- `.tmp/` 已加入 Git 忽略规则，不得使用 `git add -f` 将其提交；清理时只能删除 `.tmp/` 内容，不得以清理临时文件为由删除 `apk/`、`output/` 或 `archive/`。

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
| LSPatch | ✅ 项目内 v0.6（`java -jar` 可运行）；上游最新稳定版 v1.2，尚未替换并入项目 |
| APK 解码 | ✅ `apktool d` 成功，输出至 `apk/decoded/` |

## 5. 环境相关已知待办

> 环境/部署相关待办已统一收录至 `docs/development/planning/backlog.md`（部署/环境表），本节不再维护。

已完结（历史记录）：
- [x] **y7000 模拟器环境**（2026-08-05 实测完结：TCG ARM VM boot 25+ 分钟未完成；x86_64 转译路线 frida 不可用 + LSPatch native 高风险）→ 模拟器路线冻结，转向真机
- [x] **实体 root 手机就绪**（✅ 安卓真机 已配置 root + Zygisk-LSPosed 并真机联调）
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
8. **Gradle wrapper zip 曾被清理**：不要手写缓存哈希目录；从项目根目录执行 `scripts/build-debug.sh`，脚本会自动发现 `.gradle/wrapper/dists/` 中的 Gradle 8.11.1。
9. **AGP 依赖下载慢**（国外仓库）→ 阿里云镜像（settings.gradle.kts 已配）。
10. **zsh 通配符不展开**（2026-08-12 实测）：缓存发行版目录含随机哈希，统一使用 `scripts/build-debug.sh`，不要在命令行手写 `*/` 或缓存哈希。
11. **frida-server 重启后需 su 启动**（2026-08-12 实测）：设备重启后 `/data/local/tmp/frida-server` 需 `adb shell su -c 'nohup /data/local/tmp/frida-server >/dev/null 2>&1 &'`（root + nohup），普通 `adb shell "frida-server &"` 无权限启动失败。
12. **通知栏遮挡启动**（2026-08-12 实测）：设备重启后首屏可能是 NotificationShade（`dumpsys window` mCurrentFocus 显示），monkey 启动游戏前先 `input keyevent 4` 关闭通知栏回到桌面，否则游戏未真正启动（8088 无监听）。
13. **真机部署**：部署与验收在真机上进行；除启动弹窗前置例外使用触摸脚本外，其余完全用 API 操控。详见 §3.4。
14. **真机启动弹窗**（2026-08-27 实测）：monkey 启动后可能出现无 API 跳过的弹窗；启动流程必须追加 `ANDROID_SERIAL=<设备序列号> uv run python scripts/device/touch_automation.py --inject input click <启动弹窗坐标> 1.0`，点击后再轮询 `/api/health` 和 `/api/ui/screen`。除该弹窗前置外，不使用真机触摸坐标。

## 6. 关联文档

- 项目总览：`README.md`；目录规范与代理规则：根目录 `AGENTS.md`
- 代码结构（NDK/CMake/依赖配置说明）：`docs/development/architecture.md`
- 开发待办：`docs/development/planning/backlog.md`
