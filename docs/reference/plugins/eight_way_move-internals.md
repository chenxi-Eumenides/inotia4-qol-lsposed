# `eight_way_move` 内部实现

触屏方向摇杆八向移动的实现参考。对外行为见 [`eight_way_move.md`](eight_way_move.md)。

## 1. 分层与目录

| 目录/文件 | 职责 |
|---|---|
| `plugins/eight_way_move/plugin.cpp` | descriptor、4 个 ResourceClaim、feature、生命周期 |
| `plugins/eight_way_move/feature.{h,cpp}` | 端口/门禁、4 个入口/调用点 wrapper、backup、30 帧状态清理任务 |
| `contracts/game_ports/movement_input_port.h` | `QOL_PORT_MOVEMENT_INPUT` v1.0 |
| `game/adapter_ports/movement_input_port.{h,cpp}` | 端口实例，转发到 `game/input` |
| `game/input/eight_way_move_math.h` | 八向分类、键码映射、朝向偏好纯逻辑 |
| `game/input/eight_way_access.{h,cpp}` | KEY 键码/键表/按住位、屏幕高度、TouchHandle 偏移、控件绝对点、虚拟按键根 |
| `game/input/eight_way_state.{h,cpp}` | 触摸状态机、动态中心、自定义键码、主方向翻译、朝向偏好 |
| `game/input/eight_way_move.{h,cpp}` | `CHAR_Move` 双轴 wrapper、回滚/恢复坐标 |
| `game/input/eight_way_draw.{h,cpp}` | `UIPlayPorting_Draw` 后补画副轴箭头 |
| `tests/test_eight_way_move.cpp` | 纯数学/朝向/副轴条件单测 |
| `tests/test_eight_way_move_plugin.cpp` | descriptor、4 资源、端口、feature 单测 |

## 2. 装配

- `plugin_descriptor()` 由 `host/assembly.cpp` 的 `kBuiltInPlugins` 注册。
- feature：`eight_way_move.direction`，默认 `0`，`FeatureCategory::kGameplay`，toggle。
- 端口：`QOL_PORT_MOVEMENT_INPUT`（`34`）v1.0，私有，仅 `eight_way_move` 可声明；由 `host/plugin_registry.cpp` 注入。
- 无 HTTP、capability、JNI、ownership。

### 2.1 资源

| id | symbol | 机制 | handler |
|---|---|---|---|
| `hook:UIPlay_ButtonDirection` | `F_UIPLAY_BUTTON_DIRECTION_VMA` | `kEntryHook` | `direction_touch_wrapper` |
| `hook:GAMESTATE_PressKeyPlay` | `F_GAMESTATE_PRESSKEYPLAY_VMA` | `kEntryHook` | `press_key_play_wrapper` |
| `patch:GAMESTATE_PressKeyPlay+0x21c` | `P_GAMESTATE_PRESSKEYPLAY_CHAR_MOVE` | `kCallPatch` | `eight_way_char_move_wrapper` |
| `hook:UIPlayPorting_Draw` | `F_UI_PLAY_PORTING_DRAW_VMA` | `kEntryHook` | `porting_draw_wrapper` |

全部 private/required/fail-closed/resumable；`gate=nullptr`，随 lifecycle 安装/还原。

## 3. 关键机制

### 3.1 游戏地址

| 符号 | VMA | 用途 |
|---|---|---|
| `UIPlay_ButtonDirection` | `0xc66d4` | 方向触摸事件 |
| `GAMESTATE_PressKeyPlay` | `0x9cfc0` | 游戏内按键接收 |
| `GAMESTATE_PressKeyPlay+0x21c` | `0x9d1dc` | `bl CHAR_Move`，原字 `0x9401318B` |
| `UIPlayPorting_Draw` | `0xc87a0` | HUD/摇杆绘制 |
| `ControlObject_GetAbsolutePoint` | `0x9e6b4` | 控件绝对左上角，返回 x0/x1 |
| `InputLayer_GetVirtualKey` | `0xac594` | 虚拟按键根 |
| `ControlObject_FindByID` | `0x9faa4` | 按 ID 查找控件 |
| `KEY_nCode` | `0x71353c` | 当前键码 |
| `KEYSET_nData` | `0x713540` | 方向键码表 |
| `KEY_bPressed` | `0x7135a0` | 按住有效位 |
| `SGL_GetScreenHeight` | `0x954f8` | 逻辑屏高（真机 640） |
| `TouchHandle_GetControlOffset` | `0xa3190` | 触摸偏移（前两字段 int64 x/y） |

入口 hook 符号必须登记在 `game/access/symbols.cpp` 的 `kHookEntrySymbols`；只加 `symbol_registry.h` 不够，preflight 会报 `symbol unresolved`，插件变 `unsupported`。

### 3.2 触摸到键码

1. `UIPlay_ButtonDirection` 原函数先执行，保留按下/命中/视觉状态。
2. `eight_way_state` 读 `point + TouchHandle offset`。
3. 取 `0x15` 控件绝对左上角，中心 = `abs + 126`；真机逻辑高 `640`，显示横屏 `854x384`，不能使用固定屏幕常量。
4. 8px 死区；八向扇区使用 `2.414 = tan(67.5°)` 比率。
5. 斜向读 `lead_member()+C_DIRECTION`（`+0x6`）；当前朝向属于两个轴时作为主轴，另一轴为副轴。
6. 写自定义键码 `0x51000000 | sector` 到 `KEY_nCode`，并保存 `primary_dir/secondary_dir/primary_key/owns_input`。

### 3.3 键码翻译

`GAMESTATE_PressKeyPlay` 入口 wrapper 调 `translate_press_key`：

- 自定义键码：查 state，写回 `KEY_nCode = primary_key`，再调用原函数。必须写回，因为 `GAMESTATE_PressKey` 在 `fpPressKey` 返回后会比较 `KEY_nCode` 与 `KEY[0..3]`；不匹配会清 `KEY_bPressed`。
- 普通方向键：匹配当前主轴时保持 `trusted=true`，否则清 `trusted/owns_input`。
- 非方向键：清 `trusted/owns_input`，避免物理键/技能键误叠副轴。

真机 KEY 表为 `[-3,-4,-1,-2]`，索引 0=左、1=右、2=上、3=下。

### 3.4 双轴移动

调用点 patch 到 `eight_way_char_move_wrapper`，端口进入 `game/input/eight_way_move.cpp`：

1. `enabled/active/trusted/flag==0/secondary_dir>=0/dir==primary_dir` 全部满足才处理。
2. `ch == lead_member()`、`world_game_state_gate()==0`、`ch+0x2f0` 无路径。
3. 保存移动前 x/y。
4. 计算 `step = diagonal_step(delta)`（完整 `delta`，当前 8px → 8px），主轴 `CHAR_Move(dir, step, flag=1)` 静默移动；失败立即返回主轴错误码，不产生只走副轴的位置。
5. 主轴成功，副轴 `CHAR_Move(secondary, step, flag=0)` 完整移动。
6. 副轴失败：反向 `CHAR_Move` 回滚主轴；回滚成功后再以 `flag=0` 重走主轴；回滚失败直接恢复保存的 x/y 并返回 `2`。

主轴失败时返回原错误码，原版仍可按主轴尝试 `CHAR_MoveAsBlock`；斜向不推动。

### 3.5 双箭头绘制

`porting_draw_wrapper` 先执行原 `UIPlayPorting_Draw`，再调 `draw_secondary_arrow()`：

- 方向到隐藏子控件：下 `0x1f`、左 `0x20`、上 `0x21`、右 `0x1e`。
- 控件尺寸：`0x1e/0x20` 为 `0x46x0x5a`，`0x1f/0x21` 为 `0x5a x0x46`；中心 = 绝对左上角 + 半宽/半高。
- 点亮 loc：下 `0x10`、左 `0x0d`、上 `0x0f`、右 `0x0e`（IMGSYS group `0x16`）。
- `GRPX_DrawPart(group, x, y, loc, 2, 2, 0)`。

不能用 `0x15` 摇杆中心绘制；否则第二个箭头会落在圆心。

### 3.6 状态清理

- 每条新触摸设置 `owns_input=true`；`reset_state()` 只在 `owns_input` 时清 `KEY_nCode` 与 `KEY_bPressed`，避免误伤物理键。
- 死区/无效触点调用 `suppress_motion()`，无条件清状态、键码和按住位。
- 插件注册每 30 帧 LogicPre 任务调用端口 `poll()`；`world_game_state_gate()!=0` 时清理残留。
- disable/deactivate 调 `set_enabled(0)`、`reset()`、移除帧任务。

## 4. 数据流

```text
触屏 → UIPlay_ButtonDirection hook → eight_way_state
  → 自定义 KEY_nCode → GAMESTATE_PressKeyPlay hook
  → 主方向键码 → 原版行走/切图/拾取/动画
  → CHAR_Move 调用点 patch → 主轴静默 + 副轴完整
  → UIPlayPorting_Draw hook → 补画副轴箭头
```

## 5. 失败与恢复

- 四资源任一安装失败：整组 fail-closed，插件 failed，不保留半装状态。
- entry hook 无 backup：宿主拒绝/回滚，wrapper 不执行。
- 端口缺失/ABI 不匹配：install 失败，不安装资源。
- disable/deactivate：移除 30 帧任务、关闭端口门禁、清自定义码，资源物理还原。

## 6. 已知限制与风险

- 斜向两轴步长固定为完整 `delta`（8px → 8px），总速度约 `sqrt(2)` 倍单轴；两轴保持 8px 对齐，避免方向切换或松手时触发原版 `+0x2e0` 补步。当前不做斜向降速。
- 全向移动未实现。
- 同方向物理键与触摸主键同码时，靠 `KEY_bPressed`/键码匹配仍有理论歧义；真机需重点回归。
- 主轴成功、副轴失败时的事件/镜头依赖“回滚后重走主轴”分支；回滚失败直接恢复坐标，属于原型新增逻辑。
- 双箭头是原版绘制后的 overlay；若原版 HUD 绘制顺序改变，可能覆盖更高层元素。
- 朝向偏好读取 `lead_member()+C_DIRECTION`；8px 补步期间可能仍用上一朝向。
- 真机烟测仅 C18L v586；其他变体需重新打集成包验证。
- 完整验收未完成：松开停止、撞墙、切图/拾取/动画、物理键盘、关闭还原、不同分辨率仍待逐项验证。

## 7. 测试与验证映射

| 范围 | 证据 |
|---|---|
| 八向分类/边界/死区/键码/朝向/副轴条件 | `tests/test_eight_way_move.cpp` |
| descriptor、4 资源、端口、feature 默认关闭与 Gameplay 分类 | `tests/test_eight_way_move_plugin.cpp` |
| 构建 | `scripts/build-debug.sh` |
| 架构/层依赖/符号/资源 | `check_arch.py`、`check_layers.py`、`check_symbols.py`、`check_resources.py` |
| 真机烟测 | C18L v586：四资源 `live`；日志 `eight_way touch` / `char_move hook active` / `eight_way draw secondary=3 loc=0xe at=(212,514)` / `eight_way diagonal applied ... delta=8 step=8`；截图确认上+右双箭头，角色斜向移动 |

## 8. 已删除旧结构

无。早期设计稿中的“固定 `0x15/0x16` 中心”和“三个资源”已被动态控件中心和四个资源替代；设计背景见 `docs/refactor/eight-way-move/design.md`，不作为契约依据。
