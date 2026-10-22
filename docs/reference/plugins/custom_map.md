# `custom_map` 插件参考

## 摘要

纯消费者、叶子功能插件：在 `RES_LoadToPool` 入口只拦截严格匹配 `m<纯数字>.dat` 的路径；同名自定义文件存在且可读时，以游戏池分配器返回其原始字节，否则调用备份原函数。字体和其他资源读取不受影响。附带一个地图浏览面板（左列表、右详情与瓦片示意图），数据来自对自定义地图文件的本地解析；不提供 capability、端口、HTTP 路由或 JNI。

除特别说明，文中 `plugins/...`、`game/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`custom_map`，插件版本 `0.1.0`（`plugins/custom_map/plugin.cpp`）。descriptor `requires` UI 组件、UI 面板与 UI 确认框 capability；`optional_requires` 静态数据 capability（用于查原版地图名，缺失时显示「自定义」）。声明 1 项私有、必需、`fail_closed` 的 `kEntryHook` 资源，注册 2 个 feature 和 1 个面板 provider，并由 `host/assembly.cpp` 装配。

## 公开能力与路由

不提供 capability，不注册 HTTP 路由、事件、命令或 JNI。feature 共 2 个：

- `custom_map.load`，版本 1、`kBasicOptimization` / `kToggle`，名称「自定义地图文件加载」。Host feature 生命周期启停插件和 hook；启用开关关闭时 wrapper 直接调用原函数。
- `custom_map.open_panel`，`kEntry`，动作「打开」，打开地图浏览面板。entry 回调内检查 `custom_map.load` 门禁：未启用时经 `ui_confirmation.open_text` 弹窗提示且不打开面板；设置页对该 entry 不做视觉置灰（该效果需要修改 settings 插件，按边界约定不实施）。

## 游戏内行为

拦截点为 `RES_LoadToPool(char* path, void** out_buf, int32_t* out_size)`（VMA `0xa9d40`，dynsym 同名）。`MAP_Load`（`0x114ab0`）和 `FONT_Load`（`0xa48c0`）都调用该资源入口，因此 wrapper 只匹配完整路径 `m` + 一个或多个 ASCII 数字 + `.dat`，避免替换字体等其他资源。

对于 `m<id>.dat`，插件读取：

`/sdcard/Android/data/<包名>/files/qol/resources/map/m<id>.dat`

包名取自 `/proc/self/cmdline`；读取失败时使用默认包名 `com.com2us.inotia4.normal.freefull.google.global.android.common`。插件不创建目录。`.dat` 与 `.dat.jpg` 并存时 `.dat` 优先（哪怕 `.dat` 不可读也按 `.dat` 路径回退原函数，不尝试 `.jpg`）。`.dat` 不存在而 `.dat.jpg` 存在时，进图路径就地解密：读取 `.jpg`（要求普通文件、首字节 `0x01` 的 LZMA 容器），经游戏 `LZMA_Decode` 解码；传入的输入长度与 `buffer + 1` 指针对应，为原文件长度减 1。解码输出必须通过完整地图结构边界校验，才会写入同目录 `m<id>.dat`（先写同目录临时文件并 `fsync`，再以硬链接原子发布；输出首字节恰为 `1` 时前置 `0x00` 防二次解码）并用于本次加载。落盘失败仅记日志，不阻断本次进图；`.jpg` 文件保留不删。容器解码、地图结构校验或游戏堆分配失败时不返回自定义缓冲，回退到原 `RES_LoadToPool`。id 数字超出 `uint32_t` 时不做 jpg 转换。可接受的文件布局有两种：

1. `.dat.jpg` 容器原始字节（magic `01 00 5d 00`，LZMA1）。`MAP_Load` 检查首字节并自行解码。
2. 首字节不等于 `1` 的预解码布局；游戏在该分支把 `buffer + 1` 作为数据起始位置。

自定义文件可读时，插件通过 `MEM_Malloc` 分配缓冲、拷贝完整文件并返回 `0`，设置 `*out_buf`，且在 `out_size` 非空时设置 `*out_size`；游戏随后以 `MEM_Free` 释放该缓冲。文件不存在、打不开、读取失败、尺寸超出 `int32_t` 或游戏池分配失败时均调用备份原函数。非匹配路径不做文件系统访问或分配，立即原样转发原函数。mapId 超过 `MAPINFOBASE_STATIC_MAX_MAP_ID`（414）的文件同样放行（用户裁决：不做上限守卫），静态表查不到的 id 在面板中显示「自定义」。

### 地图浏览面板

entry `custom_map.open_panel` 触发，经 Host `ui_panels.open("custom_map")` 打开，面板生命周期遵循 `PanelProviderSpec`（enter/process/f3/f4/event）。布局为左右双栏：

- 左栏滚动列表：`custom_map_scan()` 扫描 `qol/resources/map/` 下 `m<纯数字>.dat`，逐个解析头得到 `m<id>` 行与尺寸；目录为空或无匹配文件时显示空状态提示。
- 右栏选中详情第一行：地图 id、名称（`static_data.map_name` 查原版 `MAPINFOBASE` 表，命中用原名，否则「自定义」；超长按 UTF-8 边界截断加省略号）、大小（宽×高，单位：格）。
- 右栏第二行为瓦片示意图：按解析出的 `width × height` 以 `fill_rect` 色块矩阵绘制 base layer 的 11-bit tile id（稳定哈希映射调色板色），出口格以对比色高亮。

数据加载采用分帧流水线（模式对齐 save_backup 的备份解析）：进入面板时仅列举目录建立待解析队列（首帧跳过解析），此后每帧最多处理一个任务——优先级为选中地图详情 → JPG 转换 → 转换后 `.dat` 文件头解析 → 新扫描项；列表随解析进度出现条目并显示「解析中 n/m」。孤立 `.dat.jpg`（无同 id `.dat`）在队列中显示「解密中」，转换语义与进图路径相同并复用同一转换核心，失败显示「解密失败」；已有 `.dat` 的 id 其 `.jpg` 完全忽略。详情按 mapId 缓存，以路径、文件大小和修改时间快照校验，命中则不重复解析；每次重新进入面板重扫目录并清空缓存，解析期间检测到文件变化时提示重新打开面板。

解析格式与 `RES_LoadToPool` 拦截一致：首字节 `1` 为 LZMA 容器，经游戏 `LZMA_Decode`（VMA `0xa88c4`，dynsym GLOBAL 同名，四变体同址；插件登记为 `F_LZMA_DECODE_VMA`，签名 `int32_t(void* src, int32_t src_size, void** out_buf, int32_t* out_size)`，成功返回非 0，输出缓冲由 `MEM_Malloc` 分配、须以 `MEM_Free` 释放）解码；其余按预解码布局（`buffer+1` 起，byte2=宽、byte3=高）解析。base layer 每格 2 字节、tile id 取 `((b1 & 0x07) << 8) | b2`；其后依次为 5 个 u16 配置、section 数及 feature 列表、出口数及每出口 6 字节。解析失败（截断、解码失败）不进入详情。本插件不写任何地图数据回游戏。

## 使用的 Host SDK 与 Game 面

- `QolHostSdk::log.write`：成功采用自定义文件时记录 mapId 与文件字节数。
- `QolHostSdk::capabilities.require`：安装时获取 `QolUiComponents`（版本校验后交给面板绑定）、`UiPanelsApi`（`register_provider` / `open` / `deactivate_owner` / `unregister_owner`）与 `UiConfirmationApi`（未启用提示）。
- `QolHostSdk::static_data`（可选依赖）：`map_name` 查询原版地图名。
- `F_RES_LOAD_TO_POOL_VMA` / `RES_LoadToPool`：hook target 与缺省文件读取回退。
- `F_LZMA_DECODE_VMA` / `LZMA_Decode`：仅解析地址供 `map_data` 直接调用，不声明 hook 资源。
- `F_MEM_MALLOC_VMA` / `MEM_Malloc` 与 `F_MEM_FREE_VMA` / `MEM_Free`：为地图加载提供与游戏释放器兼容的缓冲区分配/清理。符号函数指针由 `game/access/game_access.cpp` 初始化。
- 包名读取遵循 `infra/log/qol_log.cpp:245-253` 的 `/proc/self/cmdline` 与默认包名回退方式。

## 消费者与职责边界

本插件不提供能力且无下游消费者。插件拥有地图资源覆盖规则；入口 hook 安装、备份函数交还和资源回收归 Host hooks/Loader。除精确地图路径外的资源行为归原版 `RES_LoadToPool`。

## 失败语义（fail-closed）

- 资源声明为 `kRequired` / `kFailClosed`：hook 不能安装或资源回调未能保存有效 backup 时，Host 回滚资源并将插件置为失败，不运行无 backup 的 wrapper。
- wrapper 没有备份函数时返回负值；正常 fail-closed 安装路径要求 backup 有效。
- 自定义路径构造、文件打开/读取、容器解码、地图结构边界校验、尺寸检查或 `MEM_Malloc` 失败时回退到原 `RES_LoadToPool`，不将自定义文件错误伪装成成功。
- `MEM_Malloc` 已分配但文件读取失败时，先用 `MEM_Free` 释放部分读取缓冲，再回退原函数。
- 非匹配路径（包括字体加载）无文件 I/O，原始参数逐项传给 backup。

## 生命周期

- `install`：保存 Host SDK；require UI 组件/面板/确认框 capability，注册面板 provider，并把组件接口绑定到面板层；任一步失败即 install 失败并记录错误，已注册的 provider 回收。
- `on_resource_ready`：只接受 `hook:RES_LoadToPool` 和非空 backup，保存原始函数地址。
- `on_enabled` / `on_disabled`：原子更新 feature 门禁；关闭后 wrapper 直接转发，entry 弹窗提示未启用。
- `deactivate`：面板 owner 注销并关闭面板、解绑面板层、清除启用状态、backup、确认框与 SDK 指针。
- `fill_status`：报告当前门禁状态与 install 错误。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/custom_map/plugin.cpp` | descriptor、capability 依赖、资源 claim、面板 provider 注册、wrapper 入口、生命周期、backup 校验与 `custom_map.load` / `custom_map.open_panel` feature。 |
| `plugins/custom_map/feature.cpp` | 严格文件名解析、包名发现、自定义文件读取、游戏池分配/释放、原函数回退和成功日志。 |
| `plugins/custom_map/feature.h` | 资源加载器类型与业务层生命周期/入口声明。 |
| `plugins/custom_map/map_data.cpp` | 自定义地图扫描与解析：LZMA 容器解码或预解码布局、宽高、base layer tile id 与出口。 |
| `plugins/custom_map/map_data.h` | `CustomMapSummary` / `CustomMapExit` / `CustomMapDetail` 与 `custom_map_scan` / `custom_map_read_detail` 接口。 |
| `plugins/custom_map/ui/panel.cpp` | 地图浏览面板：左栏滚动列表、右栏详情与瓦片示意图绘制、空状态与选择联动。 |
| `plugins/custom_map/ui/panel.h` | 面板 enter/process/f3/f4/event 与 provider open/close/active/block 回调声明。 |
| `plugins/custom_map/CMakeLists.txt` | `qol_plugin_custom_map` 静态 target。 |
| `game/symbols/game_symbols.h` | `F_RES_LOAD_TO_POOL_VMA`、`F_LZMA_DECODE_VMA` 与游戏池 `MEM_Malloc` / `MEM_Free` VMA。 |
| `game/symbols/symbol_registry.h` | `RES_LoadToPool` dynsym 注册；`MEM_Malloc` / `MEM_Free` 已有注册项。 |
| `game/access/symbols.cpp` | `kHookEntrySymbols` 入口 hook 符号解析登记（含 `F_LZMA_DECODE_VMA` 仅解析条目）。 |
| `host/assembly.cpp` | 内建插件 include 与 descriptor 装配清单。 |
