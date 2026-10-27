# `eight_way_move` 内部实现

主控角色 1px 全向移动的实现参考。对外行为见 [`eight_way_move.md`](eight_way_move.md)。

## 1. 分层与目录

| 目录/文件 | 职责 |
|---|---|
| `plugins/eight_way_move/plugin.cpp` | descriptor、8 个 ResourceClaim、feature、生命周期 |
| `plugins/eight_way_move/feature.{h,cpp}` | 端口/门禁、6 个入口/调用点 wrapper、backup、每帧状态任务 |
| `contracts/game_ports/movement_input_port.h` | `QOL_PORT_MOVEMENT_INPUT` v1.1 |
| `game/adapter_ports/movement_input_port.{h,cpp}` | 端口实例，转发到 `game/input` |
| `game/input/eight_way_move_math.h` | 44 格点表、角度选择/滞回、主副轴拆分、速度缩放 |
| `game/input/eight_way_access.{h,cpp}` | KEY/键表/按住位、坐标/路径、碰撞查询、控件/摄像机访问 |
| `game/input/eight_way_state.{h,cpp}` | 触摸状态机、自定义键码、补步门禁、主控切换吸附 |
| `game/input/eight_way_move.{h,cpp}` | `CHAR_Move` 双轴 wrapper、滑行/回滚/恢复坐标 |
| `game/input/eight_way_draw.{h,cpp}` | `UIPlayPorting_Draw` 后按原始触摸角度绘制旋转指针 |
| `tests/test_eight_way_move.cpp` | 44 格点、角度/滞回、主副轴、缩放、自定义键码单测 |
| `tests/test_eight_way_move_plugin.cpp` | descriptor、8 资源、端口、feature 单测 |

## 2. 装配

- `plugin_descriptor()` 由 `host/assembly.cpp` 的 `kBuiltInPlugins` 注册。
- feature：`eight_way_move.direction`，默认 `0`，`FeatureCategory::kGameplay`，toggle；显示名「全向移动」。
- 端口：`QOL_PORT_MOVEMENT_INPUT`（`34`）v1.1，私有，仅 `eight_way_move` 可声明；由 `host/plugin_registry.cpp` 注入。
- 无 HTTP、capability、JNI、ownership。

### 2.1 资源

| id | symbol | 机制 | handler |
|---|---|---|---|
| `hook:UIPlay_ButtonDirection` | `F_UIPLAY_BUTTON_DIRECTION_VMA` | `kEntryHook` | `direction_touch_wrapper` |
| `hook:GAMESTATE_PressKeyPlay` | `F_GAMESTATE_PRESSKEYPLAY_VMA` | `kEntryHook` | `press_key_play_wrapper` |
| `patch:GAMESTATE_PressKeyPlay+0x21c` | `P_GAMESTATE_PRESSKEYPLAY_CHAR_MOVE` | `kCallPatch` | `eight_way_char_move_wrapper` |
| `hook:UIPlayPorting_Draw` | `F_UI_PLAY_PORTING_DRAW_VMA` | `kEntryHook` | `porting_draw_wrapper` |
| `hook:CHAR_MoveOnWaitCoord` | `F_CHAR_MOVE_ON_WAIT_COORD_VMA` | `kEntryHook` | `move_on_wait_coord_wrapper` |
| `hook:MAP_IsBlockingByPixel` | `F_MAP_IS_BLOCKING_BYPIXEL_VMA` | `kEntryHook` | `map_blocking_wrapper` |
| `patch:GAMESTATE_PressKey+0x1c0` | `P_GAMESTATE_PRESSKEY_RELEASE_ALIGN` | `kCallPatch`（word replace） | 无 |
| `patch:CHAR_SetAction+0x26c` | `P_CHAR_SET_ACTION_ALIGN` | `kCallPatch`（word replace） | 无 |

全部 private/required/fail-closed/resumable；`gate=nullptr`，随 lifecycle 安装/还原。word replace 资源没有 handler，`handler=nullptr` 是预期值。

## 3. 关键机制

### 3.1 游戏地址

| 符号 | VMA | 用途 |
|---|---|---|
| `UIPlay_ButtonDirection` | `0xc66d4` | 方向触摸事件 |
| `GAMESTATE_PressKeyPlay` | `0x9cfc0` | 游戏内按键接收 |
| `GAMESTATE_PressKeyPlay+0x21c` | `0x9d1dc` | `bl CHAR_Move`，原字 `0x9401318B` |
| `GAMESTATE_PressKey` | `0x151310` | 松手/按键释放 |
| `GAMESTATE_PressKey+0x1c0` | `0x1514d0` | 未对齐松手分支 `mov w0,#1` → `b 0x1514bc` |
| `CHAR_SetAction` | `0xe7630` | 动作切换 |
| `CHAR_SetAction+0x26c` | `0xe789c` | 未对齐动作分支 `mov w0,#1` → `b 0xe76a8` |
| `CHAR_MoveOnWaitCoord` | `0xe9cd0` | 原版 8px 补步 |
| `MAP_IsBlockingByPixel` | `0x113bcc` | 像素碰撞查询 |
| `MAP_IsBlocking` | `0x113b6c` | 16px 瓦片阻挡查询 |
| `UIPlayPorting_Draw` | `0xc87a0` | HUD/摇杆绘制 |
| `ControlObject_GetAbsolutePoint` | `0x9e6b4` | 控件绝对左上角 |
| `InputLayer_GetVirtualKey` | `0xac594` | 虚拟按键根 |
| `ControlObject_FindByID` | `0x9faa4` | 按 ID 查找控件 |
| `KEY_nCode` | `0x71353c` | 当前键码 |
| `KEYSET_nData` | `0x713540` | 方向键码表 |
| `KEY_bPressed` | `0x7135a0` | 按住有效位 |
| `SGL_GetScreenHeight` | `0x954f8` | 逻辑屏高 |
| `TouchHandle_GetControlOffset` | `0xa3190` | 触摸偏移 |
| `MAP_SetFocus` | `0x11336c` | 关闭/吸附后的摄像机焦点 |
| `G_MAP_WIDTH_GOT_VMA` | `0x2f4e60` | 地图宽 GOT 槽（双层解引用） |
| `G_MAP_HEIGHT_GOT_VMA` | `0x2f60d0` | 地图高 GOT 槽（双层解引用） |

入口 hook 符号必须登记在 `game/access/symbols.cpp` 的 `kHookEntrySymbols`；只加 `symbol_registry.h` 不够，preflight 会报 `symbol unresolved`，插件变 `unsupported`。

### 3.2 触摸到键码

1. `UIPlay_ButtonDirection` 原函数先执行，保留按下/命中/视觉状态。
2. `eight_way_state` 读 `point + TouchHandle offset`。
3. 取 `0x15` 控件绝对左上角，中心 = `abs + 126`；真机逻辑高 `640`，显示横屏 `854x384`，不能使用固定屏幕常量。
4. 8px 死区；`select_lattice_step` 用 `atan2` 选择最近表项，前后角差 ≤4° 时保持上一项，避免边界抖动。
5. `lattice_step_components` 按分量大小拆主轴/副轴；两个分量接近时优先保持当前四方向朝向。
6. 写自定义键码 `0x51000000` 到 `KEY_nCode`，保存 `lattice_index/primary_dir/secondary_dir/primary_step/secondary_step/primary_key/owns_input`。

### 3.3 键码翻译

`GAMESTATE_PressKeyPlay` 入口 wrapper 调 `translate_press_key`：

- 自定义键码：查 state，写回 `KEY_nCode = primary_key`，再调用原函数。必须写回，因为 `GAMESTATE_PressKey` 在 `fpPressKey` 返回后会比较 `KEY_nCode` 与 `KEY[0..3]`；不匹配会清 `KEY_bPressed`。
- 普通方向键：匹配当前主轴时保持 `trusted=true`，并预置朝向；否则清 `trusted/owns_input`。
- 非方向键：清 `trusted/owns_input`，避免物理键/技能键误叠。

真机 KEY 表为 `[-3,-4,-1,-2]`，索引 0=左、1=右、2=上、3=下。

### 3.4 双轴移动

调用点 patch 到 `eight_way_char_move_wrapper`，端口进入 `game/input/eight_way_move.cpp`：

1. `enabled/active/trusted/flag==0/dir==primary_dir` 全部满足才处理。
2. `ch == lead_member()`、`world_game_state_gate()==0`、`ch+0x2f0` 无路径。
3. 按 `delta/8` 缩放表项；`delta==8` 时保持表值。
4. 保存移动前 x/y。
5. 副轴 `CHAR_Move(secondary, step, flag=1)` 静默移动；返回 3 直接停住。
6. 主轴 `CHAR_Move(primary, step, flag=0)` 完整移动；成功即返回，摄像机/格子事件在最终位置执行一次。
7. 主轴被挡、副轴成功：反向 `CHAR_Move` 回滚副轴；回滚成功后以 `flag=0` 重走副轴，形成贴墙滑行。
8. 回滚或滑行失败：恢复保存的 x/y 并返回 2，避免留下静默位移。

### 3.5 碰撞查询

`MAP_IsBlockingByPixel` 的原始采样是 1/2/4 个点，并假设至少一个轴 8px 对齐；非对齐时可能漏掉一侧瓦片。hook 进入 `map_blocking_query`：

- 8px 对齐：直接调用 hook 提供的原函数 trampoline（**不能**再调用函数入口，否则会再次进入 hook 导致递归）。
- 非对齐：按角色中心所在 16px 瓦片判断；只有坐标正好落在 16px 瓦片边界时才额外检查相邻瓦片。不再把角色当成 16px 方块，避免离墙还有距离就被挡住。
- 地图宽高从 `G_MAP_WIDTH_GOT_VMA` / `G_MAP_HEIGHT_GOT_VMA` 双层解引用读取；读取失败时回退原函数。
- 不修改角色坐标。

### 3.6 补步与动作闸门

`CHAR_MoveOnWaitCoord` entry hook 进入 `handle_wait_coord`：

- 仅当 `ch == lead_member()`、无路径、Play 状态时清 `+0x2e0` 并返回 1（跳过原函数）。
- 其他角色、路径移动、非 Play 状态调用原函数。

另外两条单指令 patch 修复提前 return：

- 松手：`0x1514d0` 的 `mov w0,#1` 改为 `b 0x1514bc`，未对齐也走正常停止动作，不再保留走路动作。
- 动作切换：`0xe789c` 的 `mov w0,#1` 改为 `b 0xe76a8`，未对齐也继续设置新动作，不再被 8px 闸门吞掉。

### 3.7 主控切换吸附

每帧 LogicPre 任务调用 `poll()`：

1. `world_game_state_gate()!=0`：更新缓存的 lead，清理触摸状态。
2. Play 状态：比较 `lead_member()`；变化时对**旧主控**调用 `snap_to_tile_center`。
3. `snap_to_tile_center`：已 8px 对齐则不动；否则取当前 16px 瓦片中心 `(x/16*16+8, y/16*16+8)`，用原版碰撞查询确认可站立后直接写 x/y，并清 `+0x2e0`。
4. 直接写坐标，不调用 `CHAR_Move`，因此不触发补步、格子事件；旧主控已不是镜头目标。
5. 瓦片中心被挡或坐标不可读时跳过并记录一次 warn，交给 AI 自己的路径逻辑。

### 3.8 旋转方向指针

`porting_draw_wrapper` 先执行原 `UIPlayPorting_Draw`，再调 `draw_direction_indicator()`：

- `state.raw_active/raw_dx/raw_dy` 保存原始触摸向量（dx 右正、dy 上正）；松开、死区、非 Play 状态会清空，因此释放后不绘制。
- 摇杆中心 = `0x15` 控件绝对左上角 + `126`。
- 指针长 `96px`，尾端从中心外 `12px` 开始；箭头两翼由屏幕方向向量和垂直向量计算。
- 用 `GRPX_DrawLine`（`0x8fb7c`，ABGR8888）画主轴和两翼，用 `GRPX_FillRect` 画中心点；颜色 `0xFF00D7FF`（金色）。
- 只读原始角度，不随 44 方向量化。
- 原版四根固定箭头（0x1e..0x21）的控件矩形移到屏幕外隐藏；关闭时恢复。

## 4. 数据流

```text
触屏 → UIPlay_ButtonDirection hook → eight_way_state
  → 44 格点选择 → 自定义 KEY_nCode → GAMESTATE_PressKeyPlay hook
  → 主方向键码 → 原版行走/切图/拾取/动画
  → CHAR_Move 调用点 patch → 副轴静默 + 主轴完整
  → UIPlayPorting_Draw hook → 补画第二箭头

旧主控切走 → 每帧 poll → 瓦片中心吸附 → AI 原版 8px 路径
```

## 5. 失败与恢复

- 8 资源任一安装失败：整组 fail-closed，插件 failed，不保留半装状态。
- entry hook 无 backup：宿主拒绝/回滚，wrapper 不执行。
- 端口缺失/ABI 不匹配：install 失败，不安装资源。
- disable/deactivate：先 `set_enabled(0)` 吸附当前主控到瓦片中心，再移除每帧任务、关闭端口门禁、清自定义码，资源物理还原。

## 6. 已知限制与风险

- 单帧距离 7.62–8.25px，最大误差 4.8%；45° 最近为 39.8°/50.2°，没有精确对角线。
- 1px 网格上除轴向外不存在精确 8px 的整数向量；这是量化取舍，不是实现误差。
- 碰撞查询对非对齐坐标使用“中心所在瓦片”；贴墙距离接近原版，但斜向进角落时不再按 16px 方块提前阻挡。
- 路径/AI 仍按 8px 网格；主控从非 8px 坐标启动 `move_to` 时可能先吸附 0–7px。
- 切角色吸附是同步直接写坐标；瓦片中心被挡时跳过，AI 可能仍从非对齐坐标开始。
- 旋转指针是原版绘制后的 1px 线段 overlay；原版 HUD 绘制顺序改变时可能覆盖更高层元素，线宽/颜色需按设备观感调整。
- `GAMESTATE_PressKey` 松手 patch 与 `CHAR_SetAction` patch 在所有变体同址同字，但新增 hook/patch 仍需逐变体 preflight。
- 完整设备验收未完成：贴墙/角落、门口、传送、拾取、摄像机、水面、推动、攻击/停止、松手、切角色、多分辨率、五个变体仍待逐项验证。

## 7. 测试与验证映射

| 范围 | 证据 |
|---|---|
| 44 格点/距离/角度/滞回/主副轴/缩放/键码 | `tests/test_eight_way_move.cpp` |
| descriptor、8 资源、端口 v1.1、feature 默认关闭与 Gameplay 分类 | `tests/test_eight_way_move_plugin.cpp` |
| 构建 | `scripts/build-debug.sh` |
| 架构/层依赖/符号/资源 | `check_arch.py`、`check_layers.py`、`check_symbols.py`、`check_resources.py` |
| 真机 | 待验证；本文不宣称通过 |

## 8. 已删除旧结构

早期八向分类 `EightWayClass/classify_vector/apply_facing_preference/diagonal_step` 已删除；设计背景见 `docs/refactor/eight-way-move/design.md`，不作为契约依据。
