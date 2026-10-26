# `custom_map` 插件参考

## 摘要

纯消费者、叶子功能插件：在 `RES_LoadToPool` 入口只拦截严格匹配 `m<纯数字>.dat` 的路径；同名自定义文件存在且可读时，调用专用 Game Adapter 端口将字节复制到游戏资源堆后返回，否则调用备份原函数。字体和其他资源读取不受影响。附带一个地图浏览面板（左列表、右详情与瓦片示意图），数据来自对自定义地图文件的本地解析；不提供 capability，消费一个私有 Game Adapter 端口，不注册 HTTP 路由或 JNI。

除特别说明，文中 `plugins/...`、`game/...`、`host/...` 路径均相对 `module/app/src/main/cpp/`。

## 定位与版本

`custom_map`，插件版本 `0.1.0`（`plugins/custom_map/plugin.cpp`）。descriptor `requires` UI 组件、UI 面板与 UI 确认框 capability，并声明私有 `QOL_PORT_CUSTOM_MAP_RESOURCE` v1.0；`optional_requires` 静态数据 capability（用于查原版地图名，缺失时显示「自定义」）。另声明 1 项私有、必需、`fail_closed` 的 `kEntryHook` 资源，注册 2 个 feature 和 1 个面板 provider，并由 `host/assembly.cpp` 装配。

## 公开能力与路由

不提供 capability，不注册 HTTP 路由、事件、命令或 JNI。feature 共 2 个：

- `custom_map.load`，版本 1、`kBasicOptimization` / `kToggle`，名称「自定义地图」，描述「导入加载自定义地图」。Host feature 生命周期启停插件和 hook；启用开关关闭时 wrapper 直接调用原函数。
- `custom_map.open_panel`，`kEntry`，动作「打开」，打开地图浏览面板。entry 回调内检查 `custom_map.load` 门禁：未启用时经 `ui_confirmation.open_text` 弹窗提示且不打开面板；设置页对该 entry 不做视觉置灰（该效果需要修改 settings 插件，按边界约定不实施）。

## 使用方式

1. 将地图文件放入 `/sdcard/Android/data/<游戏包名>/files/qol/resources/map/`，文件名使用 `m<地图ID>.dat` 或 `m<地图ID>.dat.jpg`。
2. 自定义目录的 `.dat` 可放预解码地图数据，也可放游戏能识别的 LZMA 容器。`.dat.jpg` 是供插件转换的输入名；转换成功后会生成同 ID 的 `.dat`，原 `.jpg` 保留。
3. 同一 ID 的 `.dat` 与 `.dat.jpg` 同时存在时，`.dat` 优先，`.jpg` 不参与转换。进图时如果只有 `.dat.jpg`，插件会按需解密；即使不先进设置面板也会尝试转换。
4. 在设置页启用「自定义地图」。点「查看自定义地图」可查看扫描进度、地图 ID、名称、尺寸和瓦片示意图；开关关闭时点击按钮会提示先启用功能。
5. 进图时游戏请求对应 `m<地图ID>.dat`，命中自定义文件则优先加载；读取、解密、地图结构校验或游戏缓冲分配失败时回退原版资源加载器。若原版没有该 ID 的地图资源，回退原版加载器不代表一定存在可进入的原版地图。

地图名称从原版静态地图表查询；表中没有的自定义 ID 显示为「自定义」。插件不对地图 ID 强加 414 上限，但自定义地图是否能被游戏其他系统（地图信息、瓦片资源、出口、存档位置等）完整识别，仍取决于文件和游戏数据的一致性。

## 游戏内行为

拦截点为 `RES_LoadToPool(char* path, void** out_buf, int32_t* out_size)`（VMA `0xa9d40`，dynsym 同名）。`MAP_Load`（`0x114ab0`）和 `FONT_Load`（`0xa48c0`）都调用该资源入口，因此 wrapper 只匹配完整路径 `m` + 一个或多个 ASCII 数字 + `.dat`，避免替换字体等其他资源。

对于 `m<id>.dat`，插件读取：

`/sdcard/Android/data/<包名>/files/qol/resources/map/m<id>.dat`

包名取自 `/proc/self/cmdline`；读取失败时使用默认包名 `com.com2us.inotia4.normal.freefull.google.global.android.common`。插件不创建目录。`.dat` 与 `.dat.jpg` 并存时 `.dat` 优先（哪怕 `.dat` 不可读也按 `.dat` 路径回退原函数，不尝试 `.jpg`）。`.dat` 不存在而 `.dat.jpg` 存在时，进图路径就地解密：读取 `.jpg`（要求普通文件、首字节 `0x01` 的 LZMA 容器），经 Game Adapter 端口解码。解码输出必须通过完整地图结构边界校验，才会写入同目录 `m<id>.dat`（先写同目录临时文件并 `fsync`，再以硬链接原子发布；输出首字节恰为 `1` 时前置 `0x00` 防二次解码）并用于本次加载。转换文件可在分配本次游戏缓冲前落盘；若端口分配失败，本次回退原 `RES_LoadToPool`，已转换的 `.dat` 保留供后续重试。其他落盘失败仅记日志，不阻断本次进图；`.jpg` 文件保留不删。容器解码或地图结构校验失败时不写 `.dat`、不返回自定义缓冲，并回退原 loader。id 数字超出 `uint32_t` 时不做 jpg 转换。可接受的文件布局有两种：

1. `.dat.jpg` 容器原始字节（magic `01 00 5d 00`，LZMA1）。`MAP_Load` 检查首字节并自行解码。
2. 首字节不等于 `1` 的预解码布局；游戏在该分支把 `buffer + 1` 作为数据起始位置。

自定义文件可读时，插件通过 `QolCustomMapResourcePort::allocate_game_buffer` 将字节复制进游戏资源堆并返回 `0`，设置 `*out_buf`，且在 `out_size` 非空时设置 `*out_size`；成功交付后的缓冲所有权归游戏，随后由游戏以配对的释放器回收。文件不存在、打不开、读取失败、尺寸超出 `int32_t` 或端口分配失败时均调用备份原函数。非匹配路径不做文件系统访问或分配，立即原样转发原函数。mapId 超过 `MAPINFOBASE_STATIC_MAX_MAP_ID`（414）的文件同样放行（用户裁决：不做上限守卫），静态表查不到的 id 在面板中显示「自定义」。

### 地图浏览面板

entry `custom_map.open_panel` 触发，经 Host `ui_panels.open("custom_map")` 打开，面板生命周期遵循 `PanelProviderSpec`（enter/process/f3/f4/event）。布局为左右双栏：

- 左栏滚动列表：`custom_map_list_files()` 扫描 `qol/resources/map/`，队列逐项解析头得到 `m<id>` 行与尺寸；目录为空或无匹配文件时显示空状态提示。
- 右栏选中详情第一行：地图 id、名称（`static_data.map_name` 查原版 `MAPINFOBASE` 表，命中用原名，否则「自定义」；超长按 UTF-8 边界截断加省略号）、大小（宽×高，单位：格）。
- 右栏第二行为瓦片示意图：按解析出的 `width × height` 以 `fill_rect` 色块矩阵绘制 base layer 的 11-bit tile id（稳定哈希映射调色板色），出口格以对比色高亮。

数据加载采用分帧流水线（模式对齐 save_backup 的备份解析）：进入面板时仅列举目录建立待解析队列（首帧跳过解析），此后每帧最多处理一个任务——优先级为选中地图详情 → JPG 转换 → 转换后 `.dat` 文件头解析 → 新扫描项；列表随解析进度出现条目并显示「解析中 n/m」。孤立 `.dat.jpg`（无同 id `.dat`）在队列中显示「解密中」，转换语义与进图路径相同并复用同一转换核心，失败显示「解密失败」；已有 `.dat` 的 id 其 `.jpg` 完全忽略。详情按 mapId 缓存，以路径、文件大小和修改时间快照校验，命中则不重复解析；每次重新进入面板重扫目录并清空缓存，解析期间检测到文件变化时提示重新打开面板。

解析格式与 `RES_LoadToPool` 拦截一致：首字节 `1` 为 LZMA 容器，由 `QolCustomMapResourcePort::decode_lzma_container` 解码。Game Adapter 封装 `LZMA_Decode` ABI 并在返回前释放游戏堆临时缓冲，只向插件的普通 caller-owned buffer 复制解码结果；其余按预解码布局（`buffer+1` 起，byte2=宽、byte3=高）解析。base layer 每格 2 字节、tile id 取 `((b1 & 0x07) << 8) | b2`；其后依次为 5 个 u16 配置、section 数及 feature 列表、出口数及每出口 6 字节。解析失败（截断、解码失败）不进入详情。本插件不写任何地图数据回游戏。

## 使用的 Host SDK 与 Game 面

- `QolHostSdk::log.write`：成功采用自定义文件时记录 mapId 与文件字节数。
- `QolHostSdk::capabilities.require`：安装时获取 `QolUiComponents`（版本校验后交给面板绑定）、`UiPanelsApi`（`register_provider` / `open` / `deactivate_owner` / `unregister_owner`）与 `UiConfirmationApi`（未启用提示）。
- `QOL_PORT_CUSTOM_MAP_RESOURCE`：由 PluginContext 注入；`decode_lzma_container` 解码到插件提供的普通缓冲，`allocate_game_buffer` 创建交给原生游戏调用者释放的资源缓冲。VMA、decoder 函数指针和临时游戏堆缓冲均不暴露给插件。
- `QolHostSdk::static_data`（可选依赖）：`map_name` 查询原版地图名。
- `F_RES_LOAD_TO_POOL_VMA` / `RES_LoadToPool`：hook target 与缺省文件读取回退。
- 包名读取遵循 `infra/log/qol_log.cpp:245-253` 的 `/proc/self/cmdline` 与默认包名回退方式。

## 消费者与职责边界

本插件不提供能力且无下游消费者。插件拥有地图资源覆盖规则；入口 hook 安装、备份函数交还和资源回收归 Host hooks/Loader。除精确地图路径外的资源行为归原版 `RES_LoadToPool`。

## 失败语义（fail-closed）

- 资源声明为 `kRequired` / `kFailClosed`：hook 不能安装或资源回调未能保存有效 backup 时，Host 回滚资源并将插件置为失败，不运行无 backup 的 wrapper。
- wrapper 没有备份函数时返回负值；正常 fail-closed 安装路径要求 backup 有效。
- 自定义路径构造、文件打开/读取、容器解码、地图结构边界校验、尺寸检查或 Game Adapter 资源分配失败时回退到原 `RES_LoadToPool`，不将自定义文件错误伪装成成功。
- 非匹配路径（包括字体加载）无文件 I/O，原始参数逐项传给 backup。

## 生命周期

- `install`：读取并校验专用 Game Adapter port；保存 Host SDK；require UI 组件/面板/确认框 capability，注册面板 provider，并把端口与组件接口绑定到对应功能层；任一步失败即 install 失败并记录错误，已注册的 provider 回收。
- `on_resource_ready`：只接受 `hook:RES_LoadToPool` 和非空 backup，保存原始函数地址。
- `on_enabled` / `on_disabled`：原子更新 feature 门禁；关闭后 wrapper 直接转发，entry 弹窗提示未启用。
- `deactivate`：面板 owner 注销并关闭面板、解绑面板层、清除启用状态、backup、确认框与 SDK 指针。
- `fill_status`：报告当前门禁状态与 install 错误。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/custom_map/plugin.cpp` | descriptor、capability 依赖、资源 claim、面板 provider 注册、wrapper 入口、生命周期、backup 校验与 `custom_map.load` / `custom_map.open_panel` feature。 |
| `plugins/custom_map/feature.cpp` | 严格文件名解析、包名发现、自定义文件读取、端口资源缓冲交付、原函数回退和成功日志。 |
| `plugins/custom_map/feature.h` | 资源加载器类型与业务层生命周期/入口声明。 |
| `plugins/custom_map/map_data.cpp` | 自定义地图扫描与解析：经专用端口解码 LZMA 容器或读取预解码布局，提取宽高、base layer tile id 与出口。 |
| `plugins/custom_map/map_data.h` | `CustomMapSummary` / `CustomMapExit` / `CustomMapDetail` 与 `custom_map_list_files` / `custom_map_read_summary` / `custom_map_read_detail` 接口。 |
| `plugins/custom_map/ui/panel.cpp` | 地图浏览面板：左栏滚动列表、右栏详情与瓦片示意图绘制、空状态与选择联动。 |
| `plugins/custom_map/ui/panel.h` | 面板 enter/process/f3/f4/event 与 provider open/close/active/block 回调声明。 |
| `plugins/custom_map/CMakeLists.txt` | `qol_plugin_custom_map` 静态 target。 |
| `contracts/game_ports/custom_map_resource_port.h` | 解码与游戏资源缓冲交付的专用端口契约，不暴露游戏符号或临时堆指针。 |
| `game/adapter_ports/custom_map_resource_port.cpp` | 端口实现：调用 LZMA/游戏分配器，释放解码临时缓冲并转交资源缓冲。 |
| `host/plugin_registry.cpp` | 端口提供者注册及 `custom_map` owner 限制。 |
| `host/assembly.cpp` | 内建插件 include 与 descriptor 装配清单。 |
