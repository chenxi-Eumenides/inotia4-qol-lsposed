# `custom_map` 插件参考

## 摘要

纯消费者、叶子功能插件：在 `RES_LoadToPool` 入口只拦截严格匹配 `m<纯数字>.dat` 的路径；同名自定义文件存在且可读时，以游戏池分配器返回其原始字节，否则调用备份原函数。字体和其他资源读取不受影响。无 capability、端口、HTTP 路由或 JNI。

除特别说明，文中 `plugins/...`、`game/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`custom_map`，插件版本 `0.1.0`（`plugins/custom_map/plugin.cpp`）。descriptor 无 `requires`、`optional_requires`、`provides` 和端口；声明 1 项私有、必需、`fail_closed` 的 `kEntryHook` 资源，注册 1 个 feature，并由 `host/assembly.cpp` 装配。

## 公开能力与路由

不提供 capability，不注册 HTTP 路由、事件、命令或 JNI。唯一 feature 是 `custom_map.load`，版本 1、默认开启、`kBasicOptimization` / `kToggle`，名称「自定义地图文件加载」。Host feature 生命周期启停插件和 hook；启用开关关闭时 wrapper 直接调用原函数。

## 游戏内行为

拦截点为 `RES_LoadToPool(char* path, void** out_buf, int32_t* out_size)`（VMA `0xa9d40`，dynsym 同名）。`MAP_Load`（`0x114ab0`）和 `FONT_Load`（`0xa48c0`）都调用该资源入口，因此 wrapper 只匹配完整路径 `m` + 一个或多个 ASCII 数字 + `.dat`，避免替换字体等其他资源。

对于 `m<id>.dat`，插件读取：

`/sdcard/Android/data/<包名>/files/qol/custom_map/m<id>.dat`

包名取自 `/proc/self/cmdline`；读取失败时使用默认包名 `com.com2us.inotia4.normal.freefull.google.global.android.common`。插件不创建目录或解码内容。可接受的文件布局有两种：

1. `.dat.jpg` 容器原始字节（magic `01 00 5d 00`，LZMA1）。`MAP_Load` 检查首字节并自行解码。
2. 首字节不等于 `1` 的预解码布局；游戏在该分支把 `buffer + 1` 作为数据起始位置。

自定义文件可读时，插件通过 `MEM_Malloc` 分配缓冲、拷贝完整文件并返回 `0`，设置 `*out_buf`，且在 `out_size` 非空时设置 `*out_size`；游戏随后以 `MEM_Free` 释放该缓冲。文件不存在、打不开、读取失败、尺寸超出 `int32_t` 或游戏池分配失败时均调用备份原函数。非匹配路径不做文件系统访问或分配，立即原样转发原函数。mapId 超过 `MAPINFOBASE_STATIC_MAX_MAP_ID`（414）未验证，当前机制不按该上限过滤。

## 使用的 Host SDK 与 Game 面

- `QolHostSdk::log.write`：成功采用自定义文件时记录 mapId 与文件字节数。
- `F_RES_LOAD_TO_POOL_VMA` / `RES_LoadToPool`：hook target 与缺省文件读取回退。
- `F_MEM_MALLOC_VMA` / `MEM_Malloc` 与 `F_MEM_FREE_VMA` / `MEM_Free`：为地图加载提供与游戏释放器兼容的缓冲区分配/清理。符号函数指针由 `game/access/game_access.cpp` 初始化。
- 包名读取遵循 `infra/log/qol_log.cpp:245-253` 的 `/proc/self/cmdline` 与默认包名回退方式。

## 消费者与职责边界

本插件不提供能力且无下游消费者。插件拥有地图资源覆盖规则；入口 hook 安装、备份函数交还和资源回收归 Host hooks/Loader。除精确地图路径外的资源行为归原版 `RES_LoadToPool`。

## 失败语义（fail-closed）

- 资源声明为 `kRequired` / `kFailClosed`：hook 不能安装或资源回调未能保存有效 backup 时，Host 回滚资源并将插件置为失败，不运行无 backup 的 wrapper。
- wrapper 没有备份函数时返回负值；正常 fail-closed 安装路径要求 backup 有效。
- 自定义路径构造、文件打开/读取、尺寸检查或 `MEM_Malloc` 失败时回退到原 `RES_LoadToPool`，不将自定义文件错误伪装成成功。
- `MEM_Malloc` 已分配但文件读取失败时，先用 `MEM_Free` 释放部分读取缓冲，再回退原函数。
- 非匹配路径（包括字体加载）无文件 I/O，原始参数逐项传给 backup。

## 生命周期

- `install`：保存 Host SDK 并清空运行态；descriptor 不依赖 capability 或 port。
- `on_resource_ready`：只接受 `hook:RES_LoadToPool` 和非空 backup，保存原始函数地址。
- `on_enabled` / `on_disabled`：原子更新 feature 门禁；关闭后 wrapper 直接转发。
- `deactivate`：清除启用状态、backup 和 SDK 指针。
- `fill_status`：报告当前门禁状态与 install 错误。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/custom_map/plugin.cpp` | descriptor、资源 claim、wrapper 入口、生命周期、backup 校验与 `custom_map.load` feature。 |
| `plugins/custom_map/feature.cpp` | 严格文件名解析、包名发现、自定义文件读取、游戏池分配/释放、原函数回退和成功日志。 |
| `plugins/custom_map/feature.h` | 资源加载器类型与业务层生命周期/入口声明。 |
| `plugins/custom_map/CMakeLists.txt` | `qol_plugin_custom_map` 静态 target。 |
| `game/symbols/game_symbols.h` | `F_RES_LOAD_TO_POOL_VMA` 与游戏池 `MEM_Malloc` / `MEM_Free` VMA。 |
| `game/symbols/symbol_registry.h` | `RES_LoadToPool` dynsym 注册；`MEM_Malloc` / `MEM_Free` 已有注册项。 |
| `game/access/symbols.cpp` | `kHookEntrySymbols` 入口 hook 符号解析登记。 |
| `host/assembly.cpp` | 内建插件 include 与 descriptor 装配清单。 |
