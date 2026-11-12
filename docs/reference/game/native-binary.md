# 原版 libgame.so 静态画像与函数族边界

> 日期：2026-11-12 ｜ 样本：`apk/game-apk/艾诺迪亚4_原版_v1.3.2.apk`（versionName 1.3.2 / versionCode 32）
> 来源：apktool 3.0.3 解码 + jadx 1.5.6 反编译 + `readelf`/`llvm-objdump`（NDK r26d）静态统计。
> 本文只记录原版二进制的可测量事实和函数族结构，不描述模块实现，也不给移植工期估算。

## 1. 解包位置

| 产物 | 路径 | 大小 | 说明 |
|---|---|---|---|
| apktool 解码 | `apk/decoded/original/` | 169 MB | `AndroidManifest.xml`、`apktool.yml`、`smali/`、`smali_classes2/`、`res/`、`assets/`、`lib/`、`unknown/` |
| jadx 反编译 | `apk/decompiled/original/` | 103 MB | `sources/`（5796 个 `.java`）、`resources/` |
| 输入 APK | `apk/game-apk/艾诺迪亚4_原版_v1.3.2.apk` | 48,410,169 B | 2179 个 zip 条目 |

- 目录名沿用变体映射（原版 → `original`，与 `overhaul`/`monster`/`c18` 同级）。
- `apktool.yml`：`minSdkVersion 16`、`targetSdkVersion 29`、`versionCode 32`、`versionName 1.3.2`、`packageId 127`。
- `lib/` 含 `arm64-v8a` 与 `armeabi-v7a`；本文统计只针对 `arm64-v8a`。
- `assets/common/game_res` 约 38 MB，与其他变体同一资源布局（`apk/static-data/` 的导出流程不变）。
- jadx 以 `-j 8` 运行，退出码 3，日志记录 14 处反编译错误（混淆代码常见），`sources/`、`resources/` 产物完整。

## 2. ELF 静态画像（`lib/arm64-v8a/libgame.so`）

| 指标 | 数值 |
|---|---|
| 文件大小 | 3,061,784 B |
| 类型 | ELF64 AArch64 shared object，**已 strip**（无 `.symtab`，仅 `.dynsym`） |
| BuildID | `cb422ec4db480f5ff18462f56945b25d4740af74` |
| `.text` | 1,800,840 B（`0x1b7a88`），约 450,210 条指令 |
| `.rodata` | 208,216 B |
| `.data` | 16,264 B |
| `.bss` | **4,431,608 B**（约 4.23 MiB） |
| `.data.rel.ro` | 15,584 B |
| `.init_array` / `.fini_array` | 12 / 2 个条目 |
| 导出 FUNC 符号 | 6,620 个，对应 **6,218 个不同地址** |
| 导出符号覆盖 `.text` | 75%（区间并集 1,350,728 B）→ 其余约 0.45 MB 为无符号的 static/内联代码 |
| 导入符号（UND） | 250 个 |
| `NEEDED` | `libGLESv1_CM.so`、`libdl.so`、`liblog.so`、`libstdc++.so`、`libm.so`、`libc.so` |

注意：

- 导出符号 size 之和为 3.46 MB，**大于** `.text`（1.76 MB），存在别名符号和跳板，因此不能用 size 求和做代码占比；占比只用"区间并集覆盖率"和函数计数。
- 所有函数计数是**下界**：未导出函数无法从 `.dynsym` 枚举。

## 3. 函数族构成

按导出符号名前缀归类（函数数 / 符号字节数，字节数受别名影响仅供量级参考）：

| 族 | 函数数 | 字节数 | 说明 |
|---|---|---|---|
| `CHAR_` | 354 | 272 KB | 角色、战斗、属性、Buff 运算 |
| `C++/STL`（`_Z*`） | 773 | 337 KB | 内联展开的 libstdc++ 模板实例 |
| `libpng`（`png_`） | 233 | 173 KB | 图像解码 |
| `CS_` | 352 | 70 KB | 平台抽象（网络/文件/内存桥接） |
| `Scene_` | 189 | 70 KB | 场景与画面状态机（类层次重） |
| `ControlObject_` | 101 | — | 控件对象基类 |
| `SGL_` | 89 | 71 KB | 自研软件光栅/绘制层 |
| `GRP_` | 79 | 44 KB | 图形资源与绘制组 |
| `GRPX_` | 54 | — | `SGL` 之上的绘制扩展 |
| `ITEMSYSTEM_` | 75 | 67 KB | 物品系统 |
| `UI*`（`UI_` 68 / `UIMix` 52 / `UISkill` 44 / `UIEquip` 44 / `UIInApp` 44 / `UIDesc` 35 / `UIStore` 34 / `UIMercenary` 35 …，共 31 个前缀） | 612 | — | 逐面板 UI，族内重复度高 |
| `SAVE_` | 68 | 37 KB | 存档读写 |
| `MAP_` | 55 | 57 KB | 地图、瓦片、阻挡 |
| `CSOGL_` / `CSFB_` | 57 / 56 | 31 / 29 KB | GLES 包装 / 平台帧缓冲回读 |
| `SOUNDSYSTEM_` | 36 | — | 音频 |
| `MEM_` | 51 | — | 自研内存池 |
| `MATH_` / `UTIL_` / `MW_` | — / 43 / 38 | — | 数学、工具、UI 动画 |
| `Java_`（JNI） | 54 | 91 KB | native ↔ Java 桥接 |
| `json_` / `zlib` | 62 / 19 | — | cJSON / zlib |
| `SPR_` / `WORLDMAPBUILDER_` | 19 / 19 | 159 / 157 KB | 精灵批绘 / 世界地图构建（单函数体积最大） |

按"引擎/平台"与"游戏逻辑"粗分：引擎与第三方约 **2,720 个函数**，游戏逻辑约 **3,498 个函数**。

## 4. 依赖面

- 图形走 **OpenGL ES 1.x 固定管线**（`libGLESv1_CM.so`）；`CSOGL_` 是 GL 调用包装层（生成 `pact*` 存根），`SGL_`/`GRP_`/`GRPX_`/`SPR_` 在其上做绘制。
- C++ 运行时为 `libstdc++`（旧版 NDK 工具链），STL 模板大量内联进调用点。
- 导入符号 250 个，覆盖 libc（文件、时间、字符串、`mmap`）、`libm`、`liblog`、`libdl`（含 `dlopen`/`dlsym`/`dladdr`）。
- 静态分析范围内**未见 `pthread_create`**：与 `runtime.md` 记录的"单线程主循环"一致；该结论仅基于静态导入与调用面。
- Java 侧 11,281 个 smali 类，主要是 Hive / Firebase / Tencent / Com2uS SDK，JNI 入口只有 54 个，native 与 Java 的接口面很窄。

## 5. 调用与状态耦合（决定能否按函数替换）

| 事实 | 数值 |
|---|---|
| 间接调用 `blr` | 3,178 次 / 约 450k 条指令（0.7%） |
| 静态指针表中指向 `.text` 的地址 | 1,561 个（`.data.rel.ro` 的 `R_AARCH64_RELATIVE` 共 3,747 条） |
| 面积极大的全局状态 | `.bss` 4.23 MiB |
| 无调用、无全局访问的叶子函数 | 1,619 个 |
| 无调用（纯叶子）函数 | 2,129 个 |
| 不触碰 `.data`/`.bss` 的函数 | 3,559 个（占导出函数 57%） |
| 单函数 > 2 KB 的复杂函数 | 247 个 |

触碰全局数据的函数比例（导出函数口径，近似值）：`MAP_` 89%、`UIMix_` 88%、`SAVE_` 84%、`Scene_` 66%、`ITEMSYSTEM_` 63%、`SGL_` 61%、`GRP_` 56%、`CHAR_` 51%、`CS_` 43%、`CSOGL_` 32%、`MEM_` 25%、`libpng` 8%。

跨族调用很窄，族边界可静态识别，代表例子：

- `GRPX_` → `SGL_` 25 次、`GRP_` 8 次；`MW_` → `MW_` 30 次 + `GRP_` 8 次。
- `UI_` → `GRP_` 62 / `IMGSYS_` 48 / `GRPX_` 45。
- `SOUNDSYSTEM_` → `CS_` 17；`TouchHandle_` → `ControlObject_` 81。
- `SGL_` 内部：`internal` 287 / `C++` 109 / `SGL_` 47。
- `Scene_` 是唯一的间接调用密集族（580 次 `blr`），说明它由类层次 + 回调驱动。

## 6. 替换边界的静态依据

| 层 | 代表族 | 事实依据 |
|---|---|---|
| L1 纯函数 | `png_` 233、`json_` 62、`zlib` 19、`MATH_`/`UTIL_` 43、`MW_` 38 | 1,619 个函数既不调函数也不碰全局；`libpng` 只有 8% 函数触碰全局 |
| L2 子系统 | `SGL_` 89、`GRP_` 79、`GRPX_` 54、`SPR_` 19、`CSOGL_` 57、`SOUNDSYSTEM_` 36 | 族内共享绘制/音频状态，跨族接口窄（见 §5 调用计数） |
| L3 状态核心 | `MEM_` 51、`Scene_` 189、`ControlObject_` 101、`CHAR_` 354、`SAVE_` 68、`MAP_` 55、UI 族 612 | 全局触碰比例 51%–89%；`.bss` 4.23 MiB 的池与单例；`Scene_` 580 次 `blr` 与 1,561 个静态函数指针共同构成 vtable/回调面 |
| L4 平台/SDK | `CS_` 352、`CSFB_` 56、`Java_` 54、Hive/Firebase/Tencent | 平台抽象与桥接，无游戏规则语义 |

推论（结构性，不含工期）：L1 可以逐函数替换；L2 宜整族替换（族内混用新旧实现会分裂绘制/音频状态）；L3 不能拆成单函数替换——替换"读状态"函数而保留"写状态"函数会立刻产生行为分裂，必须先固定结构体布局与全局池语义；L4 应整体重写而不是逆向对齐。

## 7. 复现命令

```bash
apktool d -f -o apk/decoded/original "apk/game-apk/艾诺迪亚4_原版_v1.3.2.apk"
jadx -d apk/decompiled/original -j 8 "apk/game-apk/艾诺迪亚4_原版_v1.3.2.apk"

SO=apk/decoded/original/lib/arm64-v8a/libgame.so
readelf -SW "$SO"
readelf --dyn-syms -W "$SO" | awk '$4=="FUNC"'
tools/ndk/android-ndk-r26d/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-objdump -d --no-show-raw-insn "$SO"
```

## 8. 未证实与边界

- 未导出函数（约 25% `.text`）没有符号，函数计数与族归属只是下界；`internal` 归类的 3,624 次内部调用说明存在成规模的静态函数簇。
- 全局触碰比例由 `adrp` 落在 `.data`/`.bss` 判定，是下界（GOT 间接访问、指针传入的全局未计入）。
- "无 `pthread_create`"只说明静态导入面；运行时通过 `dlopen` 加载的库是否起线程未验证。
- 本文不涉及 v1.3.2 与其他变体的符号 VMA 对比；VMA 对齐走 `scripts/maintenance/check_symbols.py`。
