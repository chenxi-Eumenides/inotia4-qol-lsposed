# 八向/全向移动设计

状态：Host choice/模式接线、定向 Host 单测、Debug 构建与 C18L v589 真机烟测已完成；八向/全向/关闭三态、重启恢复和 selected 高亮已实测。完整移动验收仍 `NOT_ACCEPTED`（撞墙/滑行/切图/其他分辨率未逐项覆盖）。原 C18L v586 烟测只覆盖旧全向原型行为，不作为本轮证据。
2026-10-29 更新：现有 `eight_way_move.direction` 改为默认关闭的 choice，稳定值 `eight_way`/`omnidirectional`；旧 toggle bool 不迁移，缺 selection 按关闭。此前“44 方向 toggle”章节作为实现历史，当前配置与行为契约以 `docs/reference/plugins/eight_way_move*.md` 为准。
2026-10-29 更新：修复主控平 A 自动接近时“乱跑”问题。`CHAR_MoveAsPath` 在路径方向与当前朝向不同且主控非 8px 对齐时置 `+0x2e0`；原版 `CHAR_MoveOnWaitCoord` 只沿旧朝向补一个轴，导致先朝错误方向移动。现行 `handle_wait_coord` 对主控路径改为吸附到当前 16px 瓦片中心并跳过单轴补步；细节见 `docs/reference/plugins/eight_way_move-internals.md` §3.6。
范围：Android 触屏虚拟方向摇杆；不改键盘、AI、寻路和其他移动调用点。

## 0. 当前 choice 模式约束（2026-10-29）

- 关闭 → 八向 → 全向由 Host choice callback 通知业务；`eight_way_move.direction` ID 保持不变，option value 为 `eight_way`、`omnidirectional`。空 selection/旧配置只表示关闭，不新增迁移。
- `QOL_PORT_MOVEMENT_INPUT` 升至 v1.2，只在既有 API 尾部追加 `set_mode` 和 mode enum。旧 `set_enabled(true/false)` 兼容语义不变：true 仍是全向、false 是关闭。
- 八向只选固定表 `(±8,0)`、`(0,±8)`、`(±6,±6)`，使用现有最近方向/滞回、主副轴、1px 网格、碰撞和回退链；全向保持原 44 项表。八向对角约 `8.485px`，比 8px 轴向快约 6%，不加等速算法。
- 模式切换/关闭/触摸无效都会清 candidate index、输入状态和 `selected_dx/dy`。selected 存未缩放整数候选向量；绘制角度与轨道位置使用 selected，而不是 raw 触摸向量或碰撞后位移。
- Host 定向测试覆盖数学表、choice seed 的旧 bool 关闭修复、callback mode 映射、mode reset、port v1.2 与旧 setter。C18L v589 真机烟测确认旧 `direction=true` 升级后为关闭、八向选择重启后恢复、`mode=1 candidate=7 steps=6/6`、selected 高亮按 45° 绘制、全向保留连续角度、关闭恢复原版四箭头；撞墙/滑行/切图/其他分辨率仍未验收。
- 可复跑 Host 命令：`cmake -S module/app/src/main/cpp/tests -B .tmp/movement-choice/host`；`cmake --build .tmp/movement-choice/host --target eight_way_move_tests eight_way_move_plugin_tests host_feature_controller_tests production_feature_facade_tests host_adapter_tests feature_reconciler_tests plugin_registry_tests plugin_lifecycle_tests -j2`；`ctest --test-dir .tmp/movement-choice/host --output-on-failure -R '^(eight_way_move_tests|eight_way_move_plugin_tests|host_feature_controller_tests|production_feature_facade_tests|host_adapter_tests|feature_reconciler_tests|plugin_registry_tests|plugin_lifecycle_tests)$'`。结果 `8/8 PASS`，`git diff --check` 通过；完整输出 `.tmp/movement-choice/host/final-host-build.log` 与 `final-host-ctest.log`，host-only，不构成真机验收。

## 1. 结论先行

- 原版方向触摸只有 **4 个方向**（上/下/左/右），斜向触点被主轴规则压成最近的基本方向；45° 平局归水平。
- 原版**没有可用的“点击地图寻路移动”**。`TouchGameObject_EventMap` 里的 `CHAR_SearchPathAsLocationAndNotify` 不是触屏下的移动入口；`UIPlay_Create(1)` 会关闭地图/角色/物品输入层，Android 触屏只保留虚拟摇杆。
- 可以拿到触摸点相对命中区的坐标：`UIPlay_ButtonDirection` 的第三个参数就是 `{x,y}` 指针。加上 `TouchHandle` 偏移后，可以相对 `0x15`（可见摇杆）或 `0x16`（扩大命中区）的中心计算。
- 推荐方案：**自定义方向键码 + 原版按键接收链 + 只在 `CHAR_Move` 调用点增加第二轴**。
  - 触摸层把八向扇区编码成自定义键码，写入 `KEY_nCode`。
  - `GAMESTATE_PressKeyPlay` 入口把自定义键码翻译成原版主方向键码，并把主方向键码写回 `KEY_nCode`，再调用原函数。
  - 原版 `GAMESTATE_PressKeyPlay` 完整执行：行走动作、8px 转向闸门、切图检测、拾取、`CHAR_Move`、推动、动画。
  - 只在 `GAMESTATE_PressKeyPlay+0x21c` 的 `bl CHAR_Move` 调用点加一层 wrapper，斜向时先静默移动主轴、再完整移动副轴。
- 这样不复制原版移动分支，逻辑遗漏面最小；自定义键码只承担“触摸意图 → 键码接收器”的传输。

## 2. 原版事实与约束

### 2.1 触摸层

`STATE_EnterGame`（`0x1511a0`）固定调用 `UIPlay_Create(1)`（`0xc69c8`），触屏模式创建虚拟摇杆：

| 控件 | 矩形 | 说明 |
|---|---|---|
| `0x15` | `(10, H-252, 252, 252)` | 可见摇杆；Proc = `UIPlay_ButtonDirection`（`0xc66d4`） |
| `0x16` | `(10, H-380, 380, 380)` | 扩大命中/拖动区；自身无 Proc、`SetActive(0)` |

`UIPlay_ButtonDirection` 分支：

- `1`：按下。置 `UIPlay_bPressedDirection=1`、`KEY_bPressed(0x7135a0)=1`、清 `KEY_bReleased`，计算方向并 `KEY_SetCode`。
- `0x40`：按住拖动。仅当 `UIPlay_bPressedDirection != 0` 时重算；拖出 `0x16` 则停止。
- `2/8`：抬起；`4`：取消/移出。
- `0x10/0x20/0x100`：不产生方向移动。

原版方向计算：

```
dx = offset.x + point.x - (rect15.x + rect15.w/2)
dy = (rect15.y + rect15.h/2) - (offset.y + point.y)   // 上为正
if |dx| >= |dy|:  dx >= 0 ? 右 : 左
else:             dy > 0  ? 上 : 下
```

`0x15` 创建参数中心为 `(136, H-126)`；`0x16` 为 `(200, H-190)`。原版用 `0x15` 的**绝对**中心，`0x16` 只做命中测试。真机上逻辑高度 `H=640` 且虚拟按键根有父偏移，实际实现必须用 `ControlObject_GetAbsolutePoint(ctrl)+126`，不能用固定常量。

### 2.2 原版方向枚举

`CHAR_Move(ch, dir, delta, flag)`：

| dir | 位移 |
|---|---|
| 0 | `y += delta`（下） |
| 1 | `x -= delta`（左） |
| 2 | `y -= delta`（上） |
| 3 | `x += delta`（右） |

`CHAR_Move` 只移动一个轴，不更新朝向、不启动行走动作；调用方负责 `CHAR_SetDirection` 和 `CHAR_StartActionID(ch,2)`。

### 2.3 原版移动链

`GAMESTATE_PressKeyPlay`（`0x9cfc0`）方向键分支：

1. 非行走动作时 `CHAR_StartActionID(ch, 2)`。
2. 键码映射到 `dir`。
3. 转向闸门：当前朝向相同，或 `(x & 7) == 0 && (y & 7) == 0`，才允许 `CHAR_SetDirection`；否则置 `ch+0x2e0=1`，本帧不移动，等 `CHAR_Process` 用 `CHAR_MoveOnWaitCoord` 补到 8px 对齐点。
4. `CHAR_SetTarget(ch,0)`、`GAMEPLAY_GoMapLinkByChar(ch, x>>4, y>>4, 1)`。
5. `CHAR_Move(ch, dir, CHAR_GetAttr(ch,0x5b), 0)`。
6. 失败时按返回值处理；`CHAR_MoveAsBlock` 负责推动。
7. 更新行走动画计数 `ch+0x299`。

### 2.4 `KEY` 表与按键接收

- `KEY_nCode`：`0x71353c`。
- `KEY` 表：`0x713540`，`KEY[0]=左`、`[1]=右`、`[2]=上`、`[3]=下`。
- `STATE_ProcessGame`（`0x151540`）每帧 → `GAMESTATE_PressKey`（`0x151310`）→ `GAMESTATE_fpPressKey`（= `GAMESTATE_PressKeyPlay`）。
- `GAMESTATE_PressKey` 在调用 `fpPressKey` 后会把 `KEY_nCode` 与 `KEY[0..3]` 比较；**不匹配就清 `KEY_bPressed(0x7135a0)`（按住有效位）**。因此自定义键码在 `PressKeyPlay` 返回前必须写回主方向键码，否则按住一帧后运动停止。

## 3. 方案总览

```text
UIPlay_ButtonDirection 入口 hook
  └─ 原函数先执行（保留按下态/命中/视觉）
  └─ 计算触点相对命中区中心的 dx/dy
  └─ 8 向分类 → 主方向、副方向
  └─ 写自定义键码 KEY_nCode = QOL_DIR_BASE | sector
  └─ 保存 state

GAMESTATE_PressKeyPlay 入口 hook
  └─ 读 key
  └─ 自定义键码 → 查 state，写回主方向键码，调用原函数
  └─ 原函数完整跑完原版移动链

GAMESTATE_PressKeyPlay+0x21c 的 bl CHAR_Move 调用点 patch
  └─ 非斜向/非可信状态 → 直接调原 CHAR_Move
  └─ 斜向 → 先 CHAR_Move(主轴, delta, flag=1)，再 CHAR_Move(副轴, delta, flag=0)
```

### 3.1 为什么不在自定义键码处理里自己写移动

如果 `PressKeyPlay` 的 hook 直接复刻“设朝向、转向闸门、切图、拾取、`CHAR_Move`、推动、动画”，就会与原版分支形成第二套逻辑，后续原版行为变化或遗漏都会变成 bug。
自定义键码只做翻译，原函数继续跑；唯一的移动增量放在 `CHAR_Move` 调用点。

### 3.2 为什么不在 `GAMESTATE_PressKey` 里翻译

`GAMESTATE_PressKey` 负责 `KEY_bReleased`、`KEY_bPressed(0x7135a0)`、状态门和 `fpPressKey` 调用，翻译放在这里也能做；但 `PressKeyPlay` 是实际的方向键接收器，参数就是键码，写回 `KEY_nCode` 的时机也更靠近后置检查。选择 `PressKeyPlay` 入口，避免影响 `GAMESTATE_PressKey` 的其他按键路径。

### 3.3 为什么只 patch `bl CHAR_Move` 调用点

`CHAR_Move` 还被 `CHAR_MoveAsBlock`、`CHAR_MoveAsPath`、`CHAR_MoveOnWaitCoord`、AI 使用。全局 hook `CHAR_Move` 需要复杂的调用者判定；只改 `GAMESTATE_PressKeyPlay+0x21c` 这一个玩家方向键调用点，天然不影响 AI/寻路/推动内部调用。

## 4. 触摸层设计

### 4.1 坐标系

从 `UIPlay_ButtonDirection` 第三个参数读 `int64_t point[2]`。
`TouchHandle_GetControlOffset()`（`0xa3190`）返回的全局偏移前两个 64 位字段是 `offset.x/offset.y`。

原型实现使用 **`0x15` 可见摇杆的绝对中心**，而不是固定屏幕常量：

```
abs = ControlObject_GetAbsolutePoint(ctrl);   // 0x9e6b4，递归父链，返回 {int64 x,y}
cx = abs.x + 126;                              // 0x15 为 252x252
cy = abs.y + 126;
dx = offset.x + point.x - cx;
dy = cy - (offset.y + point.y);                // 上为正
```

原因：真机上游戏逻辑高度是 `640`，显示是横屏 `854x384`；虚拟按键根还有父控件偏移。固定 `(136, H-126)` 或 `(200, H-190)` 都会算错中心，斜向会被压成上下左右。`ControlObject_GetAbsoluteRect` 是 x8 sret ABI，C++ 不能直接调；`GetAbsolutePoint` 返回 x0/x1 两个 int64，可以正常调用。

也可以使用 **`0x16` 命中区中心**（`abs16 + 190`），但命中区中心与可见摇杆中心不同；此时必须用自定义键码覆盖主方向，否则原版主方向（按 `0x15` 算）会和八向扇区不一致。原型选择 `0x15` 动态中心，优先保证与原版主方向一致。

### 4.2 死区与八向扇区

建议小死区 `8px`（原版没有死区，中心点会判成右；八向需要避免中心抖动）。命中死区时必须显式写 `KEY_nCode=0` 停住，不能只清模块状态：原触摸回调已经先写入普通方向键码。拖出死区时原回调的 `0x40` 分支会重新置按住位，下一次触摸 hook 恢复自定义码。

```
if dx*dx + dy*dy < 8*8: 停止（写 0，清 state）
```

八向扇区按 45° 划分，`2.414 = tan(67.5°)`：

```
ax = |dx|; ay = |dy|
if ax > 2.414*ay       -> 纯水平（E/W）
else if ay > 2.414*ax  -> 纯垂直（N/S）
else                   -> 斜向（NE/SE/SW/NW）
```

斜向时主方向取主轴（`|dx| >= |dy|` 取水平，平局归水平，和原版一致），副方向取另一轴。

扇区索引 `sector` 0..7，建议 N=0、NE=1、E=2、SE=3、S=4、SW=5、W=6、NW=7。映射：

| sector | 主方向 | 副方向 |
|---|---|---|
| N | 上(2) | 无 |
| NE | 水平占优→右(3)，否则上(2) | 另一轴 |
| E | 右(3) | 无 |
| SE | 水平占优→右(3)，否则下(0) | 另一轴 |
| S | 下(0) | 无 |
| SW | 水平占优→左(1)，否则下(0) | 另一轴 |
| W | 左(1) | 无 |
| NW | 水平占优→左(1)，否则上(2) | 另一轴 |

主方向键码：`KEY[table_index]`，table_index 为 左=0、右=1、上=2、下=3。

### 4.3 触摸状态机

```
Idle
  └─ event 1/0x40 且原函数 press-active=1 且离开死区 → Active(sector)
Active
  ├─ event 1/0x40 重新算 → Active(new sector)
  ├─ event 2/4/8 或 press-active=0 → Idle
  └─ 拖出命中区 → Idle
```

状态字段：

```
enabled
active
trusted          // 当前主键来自自定义键码翻译或其后继帧
sector
primary_dir      // 0..3
primary_key      // KEY 表原值
secondary_dir    // -1 = 无
dx, dy           // 原始向量，供全向模式使用
```

`trusted` 的作用：防止玩家按住摇杆时又按物理方向键/其他键，导致副轴被错误叠加。只有 `trusted=true` 且 `dir == primary_dir` 才加副轴。

## 5. 自定义键码

### 5.1 编码

```
QOL_DIR_BASE = 0x51000000
code = QOL_DIR_BASE | sector          // 八向
// 全向可扩展：QOL_DIR_BASE | 0x100 | vector_id
```

`0x51000000` 远高于 Android keycode 和游戏内部负键码；不会与原版 `KEY` 表匹配。
`GAMESTATE_PressKey` 对未知键码不会命中任何快捷键比较，会正常调用 `fpPressKey`；`PressKeyPlay` 入口 hook 在它做任何比较前拦截。

### 5.2 翻译规则

`GAMESTATE_PressKeyPlay` 入口 wrapper：

```
if key is custom:
    if !state.active: 写 KEY_nCode=0; backup(0); return
    写 KEY_nCode = state.primary_key
    state.trusted = true
    backup(state.primary_key)
else:
    if key is one of KEY[0..3]:
        if state.active && state.trusted && key == state.primary_key:
            保持 state
        else:
            state.trusted = false
    else:
        state.trusted = false
    backup(key)
```

关键点：**写回 `state.primary_key` 必须在 `backup()` 返回前完成**，因为 `GAMESTATE_PressKey` 在 `backup()` 返回后会读 `KEY_nCode` 判断是否清 `KEY_bPressed(0x7135a0)`。写回后它看到的是原版方向键，按住状态保持。

### 5.3 清理

- 触摸抬起/取消/拖出：清 `state.active/trusted`；仅当 `state.owns_input`（当前键码/按住位由本功能写入）时写 `KEY_nCode=0` 并清按住位。不能只判断“是否自定义码”：自定义码可能已经在上一帧被翻译成普通主方向码，残留的普通码 + 按住位会在回到 Play 后继续移动；但也不能无条件清，否则会误伤同时按住的物理键。
- 死区/无效触点：清 state 并显式写 `KEY_nCode=0`。原触摸回调已经先写入普通方向键码，只清 state 不能阻止本帧 `CHAR_Move`；写 0 后 `GAMESTATE_PressKey` 的后置检查会清按住位。拖出死区时原触摸回调的 `0x40` 分支会重新置按住位，下一次触摸 hook 再写自定义码。
- 插件 disable/deactivate：`set_enabled(false)` + `reset()`，清 state，必要时写 0。
- 游戏状态切到非 play：`GAMESTATE_PressKeyPlay` 不会被调用，自定义码可能残留。插件注册每 30 帧的逻辑帧任务调用端口 `poll()`；`poll` 在 `world_game_state_gate()!=0` 时清 state/残留码。该任务不是移动逻辑，只做状态清理。

## 6. 移动层设计

### 6.1 调用点

```
GAMESTATE_PressKeyPlay + 0x21c  (0x9d1dc)
原指令字：0x9401318B  (bl 0xe9808 CHAR_Move)
```

在 `game/access/patches.cpp` 注册：

```
{"P_GAMESTATE_PRESSKEYPLAY_CHAR_MOVE",
 F_GAMESTATE_PRESSKEYPLAY_VMA, 0x21c, 0x9401318B, 0,
 PatchKind::kBlRedirect}
```

wrapper 签名与原 `CHAR_Move` 一致：

```
int32_t eight_way_char_move(void* ch, int32_t dir, int32_t delta, int32_t flag);
```

### 6.2 斜向执行

```
if !enabled || !state.active || !state.trusted: 原 CHAR_Move
if flag != 0: 原 CHAR_Move                 // 静默/非方向键调用不叠加
if state.secondary_dir < 0: 原 CHAR_Move
if dir != state.primary_dir: 原 CHAR_Move
if ch != lead_member(): 原 CHAR_Move
if gamestate_state() != 0: 原 CHAR_Move
if *(void**)((char*)ch + 0x2f0) != nullptr: 原 CHAR_Move   // 有寻路路径

r_pri = CHAR_Move(ch, dir, delta, 1)
if r_pri == 3: return 3
if r_pri != 0: return r_pri
r_sec = CHAR_Move(ch, state.secondary_dir, delta, 0)
return 0   // 主轴已移动；副轴被挡时也按成功返回，避免原版再次推动
```

设计意图：

- 主轴 `flag=1`：静默移动，跳过推动、摄像机、水面、格子事件，避免经过侧格时触发侧格切图/事件。
- 副轴 `flag=0`：完整移动，最终格执行摄像机、事件、水面检查。
- 主轴失败立即停住，不产生“只走副轴”的最终位置；这样不会停在侧格出口/事件格上等待下一帧原版切图检查。
- 朝向仍由原版在调用 `CHAR_Move` 前设置为主方向；斜向没有独立朝向，四向朝向可接受。
- 行走动作、拾取、动画计数、切图检测都只由原版执行一次，不会因斜向翻倍。
- 推动：主轴 `flag=1` 不推；主轴被挡时 wrapper 返回主轴错误码，原版可以按主轴尝试 `CHAR_MoveAsBlock`。斜向推动没有原版语义，建议先不做。

### 6.3 返回值语义

| 情况 | 返回 | 结果 |
|---|---|---|
| 主轴成功，副轴成功 | 0 | 正常斜向移动 |
| 主轴成功，副轴地图/单位阻挡 | 0 | 回滚主轴后以完整模式重走主轴，单轴终点执行完整检查 |
| 主轴成功，副轴阻挡且回滚失败 | 2 | 直接恢复移动前坐标，跳过一次推动 |
| 主轴地图/单位阻挡 | 主轴返回码 | 只走主轴或不走；原版可能按主轴尝试推动 |
| 主轴返回 3 | 3 | 角色不可移动，不再尝试副轴 |
| 副轴返回 3 | 0 | 主轴已移动，按成功返回 |

### 6.4 速度与 8px 对齐

- 当前实现两轴都用完整 `delta`：斜向实际速度 `sqrt(2) * delta`，8px 时约 1.414 倍单轴。两轴 8px 保持原版 8px 对齐，不会因斜向步长产生 `+0x2e0` 补步。
- 曾尝试两轴按 70% 缩放（8px → 6px，总速度约 1.06 倍单轴），但非 8px 坐标会在方向切换和松手时触发 `CHAR_MoveOnWaitCoord`，表现为移动变慢和松手滑行，已回退。
- 斜向降速若重新启用，必须同时处理原版 8px 转向/释放闸门，放到全向阶段单独验收。

### 6.5 斜向双箭头与朝向

**双箭头显示**：原版 `UIPlayPorting_Draw` 遍历虚拟按键子控件 0x1e..0x21，按 `KEY_nCode` 匹配各自键码后点亮对应箭头，一帧只能匹配一个键码。原型在 `UIPlayPorting_Draw` 入口 hook 中先执行原函数，再由 `game/input/eight_way_draw.cpp` 补画副轴箭头：

- 取对应方向子控件（0x1e..0x21）的绝对左上角，加该控件半宽/半高得到箭头中心；不能直接用 0x15 摇杆中心，否则第二个箭头会画在圆心；
- `IMGSYS_GetGroup(0x16)` + `IMGSYS_GetLoc(0x16, lit_loc)`；
- `GRPX_DrawPart(group, x, y, loc, 2, 2, 0)`。

方向到子控件：下→0x1f、左→0x20、上→0x21、右→0x1e。控件尺寸：0x1e/0x20 为 `0x46x0x5a`，0x1f/0x21 为 `0x5a x0x46`。

真机 KEY 表为 `[-3,-4,-1,-2]`，绘制栈按 child 0..3 使用 `[-4,-2,-3,-1]`，即 child 顺序为 右/下/左/上。点亮 loc：

| 方向 | lit loc |
|---|---|
| 下(0) | `0x10` |
| 左(1) | `0x0d` |
| 上(2) | `0x0f` |
| 右(3) | `0x0e` |

**朝向**：角色只有 4 个基础朝向，斜向时优先保持角色当前朝向，只要它属于该斜向的两个轴：

- 当前朝右 → 右上/右下仍朝右；
- 当前朝上 → 右上/左上仍朝上；
- 当前朝向不属于该斜向的两个轴 → 回退到主轴判定。

实现上在触摸分类后读取 `lead_member()+C_DIRECTION`，调用 `apply_facing_preference()` 把 `primary_dir` 换成当前朝向，并把另一轴设为副方向；原版 `CHAR_SetDirection` 因此保持当前朝向。

## 7. 全向移动扩展

### 7.1 输入

`state.dx/dy` 保存原始向量。自定义码可以只表示“全向模式 active”，主方向仍由主轴选出，供原版朝向/动画使用。

### 7.2 位移

在 `CHAR_Move` wrapper 中按向量分解：

```
len = sqrt(dx*dx + dy*dy)
step_x = round(delta * |dx| / len)
step_y = round(delta * |dy| / len)
```

按符号选择左/右、上/下，先主轴 `flag=1`，再副轴 `flag=0`。
若 `step_x/step_y` 为 0 则跳过；主轴取较大分量，保证朝向稳定。

### 7.3 关键阻塞：8px 转向闸门

原版在 `GAMESTATE_PressKeyPlay` 的 `0x9d168/0x9d174` 检查 `x&7`、`y&7`；非对齐时置 `+0x2e0` 并**提前 return，不调用 `CHAR_Move`**，随后 `CHAR_Process` 用 `CHAR_MoveOnWaitCoord` 补步。全向步长不是 8 的倍数时，这套机制会和任意角度移动互相打架。

可选路线：

1. **量化全向**：只允许 8px 步长，仍是八向；不是真正全向。
2. **绕过转向闸门**：对自定义全向输入，把 `0x9d168/0x9d174` 的 `cbnz` 分支改为不跳转（或加一个 wrapper 条件跳过），允许任意像素位置转向；需要验证动画、碰撞、拾取、摄像机。风险最高，单独验收。
3. **保留八向**：第一期只做八向；全向作为后续实验特性，不与八向同批上线。

建议第一期只交付八向；全向单独设计、单独验收。

## 8. 分层与文件改动

### 8.1 新插件

`module/app/src/main/cpp/plugins/eight_way_move/`

| 文件 | 职责 |
|---|---|
| `plugin.cpp` | descriptor、4 个 ResourceClaim、feature、生命周期 |
| `feature.h/.cpp` | 三个 wrapper、backup 原子、port 指针、gate、reset |
| `CMakeLists.txt` | `qol_plugin_eight_way_move` |
| `settings_contribution.cpp` | 空贡献，与现有插件一致 |

以下 feature 定义属于初版八向 toggle 设计历史；当前已替换为 choice，见本文 §0 与 `docs/reference/plugins/eight_way_move.md`。

feature：`eight_way_move.direction`，默认 `false`，分类 `Gameplay`，toggle，名称「八向移动」，描述「允许斜向移动」。

ResourceClaim：

| id | symbol | 机制 | handler |
|---|---|---|---|
| `hook:UIPlay_ButtonDirection` | `F_UIPLAY_BUTTON_DIRECTION_VMA` | `kEntryHook` | `direction_touch_wrapper` |
| `hook:GAMESTATE_PressKeyPlay` | `F_GAMESTATE_PRESSKEYPLAY_VMA` | `kEntryHook` | `press_key_play_wrapper` |
| `patch:GAMESTATE_PressKeyPlay+0x21c` | `P_GAMESTATE_PRESSKEYPLAY_CHAR_MOVE` | `kCallPatch` | `eight_way_char_move_wrapper` |
| `hook:UIPlayPorting_Draw` | `F_UI_PLAY_PORTING_DRAW_VMA` | `kEntryHook` | `porting_draw_wrapper` |

全部 private/required/fail-closed/resumable；`gate=nullptr`，跟随 lifecycle。

### 8.2 新 Game Host 端口

插件不能 include `game/`，也不能直接读写 `KEY_nCode`。新增窄端口：

`contracts/game_ports/movement_input_port.h`

```
QolPortId::QOL_PORT_MOVEMENT_INPUT = 34
kQolPortMovementInputVersion = {1,0}

struct QolMovementInputApi {
    void (*on_direction_touch)(void* ctrl, uint64_t event, const void* point,
                               uint64_t param3);
    int32_t (*translate_press_key)(int32_t key);   // 写回 KEY_nCode，返回要传给原函数的键码
    int32_t (*char_move)(void* ch, int32_t dir, int32_t delta, uint8_t flag);
    void (*draw_overlay)();   // UIPlayPorting_Draw 返回后补画副轴箭头
    void (*set_enabled)(uint8_t enabled);
    void (*reset)();
    void (*poll)();           // 非 Play 状态清理
};
```

- `on_direction_touch`：读触点/偏移/控件绝对中心，计算八向 state，写自定义键码。
- `translate_press_key`：自定义码 → 主方向键码并写回 `KEY_nCode`；非自定义码原样返回。
- `char_move`：内部调用原 `fn_char_move`，执行 6.2 的两轴逻辑。
- `draw_overlay`：执行 6.5 的副轴箭头绘制。
- `set_enabled/reset/poll`：feature gate、状态清理与残留码清理。

`game/adapter_ports/movement_input_port.{h,cpp}` 提供端口实例；`host/plugin_registry.cpp` 注册 `QOL_PORT_MOVEMENT_INPUT`；`contracts/game_ports/ports.h` 增加枚举、版本、`port_id_name`。

### 8.3 游戏层实现

游戏层按职责拆分：

- `game/input/eight_way_move_math.h`：八向分类、键码映射、朝向偏好纯逻辑。
- `game/input/eight_way_access.{h,cpp}`：`KEY_nCode` / `KEY` 表 / `KEY_bPressed(0x7135a0)`、屏幕高度、TouchHandle 偏移、`ControlObject_GetAbsolutePoint`、虚拟按键根/`FindByID(0x15)`。
- `game/input/eight_way_state.{h,cpp}`：触摸状态机、动态中心、自定义键码、主方向翻译、`lead_member()+C_DIRECTION` 朝向偏好。
- `game/input/eight_way_move.{h,cpp}`：`CHAR_Move` 双轴 wrapper、回滚/恢复坐标。
- `game/input/eight_way_draw.{h,cpp}`：`UIPlayPorting_Draw` 后的副轴箭头补画。
- `game/adapter_ports/movement_input_port.{h,cpp}`：端口实例。

真机逻辑高度为 `640`、显示为横屏 `854x384`，且虚拟按键根有父偏移，因此不能用固定几何；`ControlObject_GetAbsoluteRect` 是 x8 sret ABI，改用 `ControlObject_GetAbsolutePoint(ctrl)+126`。

### 8.4 符号与 patch 登记

`game/symbols/game_symbols.h` / `symbol_registry.h` 新增：

```
F_UIPLAY_BUTTON_DIRECTION_VMA = 0xc66d4
F_GAMESTATE_PRESSKEYPLAY_VMA = 0x9cfc0
F_UI_PLAY_PORTING_DRAW_VMA = 0xc87a0
F_CONTROL_OBJECT_GET_ABSOLUTE_POINT_VMA = 0x9e6b4
F_INPUT_LAYER_GET_VIRTUAL_KEY_VMA = 0xac594
F_CONTROL_OBJECT_FIND_BY_ID_VMA = 0x9faa4
G_KEY_CODE_VMA = 0x71353c
G_KEY_BPRESSED_VMA = 0x7135a0
G_KEY_TABLE_VMA = 0x713540
G_KEY_BPRESSED_VMA = KEY_bPressed(0x7135a0)
G_UIPLAY_BPRESSED_DIRECTION_VMA = 0x307081
F_SGL_GET_SCREEN_HEIGHT_VMA = 0x954f8
F_TOUCH_HANDLE_GET_CONTROL_OFFSET_VMA = 0xa3190
```

`game/access/patches.cpp` 增加 `P_GAMESTATE_PRESSKEYPLAY_CHAR_MOVE`（0x21c，原字 0x9401318B）。

`host/assembly.cpp` 增加 `#include "plugins/eight_way_move/plugin.h"` 与 descriptor 注册。

## 9. 失败与回滚

- 四个资源任一安装失败：整组 fail-closed，插件 failed，不保留半装状态。
- entry hook 的 `backup=nullptr`：宿主拒绝/回滚，wrapper 不执行。
- port 缺失/ABI 不匹配：install 失败，不安装任何资源。
- disable/deactivate：`set_enabled(false)`、`reset()`、清自定义键码，资源事务物理还原。
- 任何 wrapper 在 gate 关闭或 port 为空时：entry hook 直接调用 backup；call patch 不会存在（资源已还原），若竞态窗口内被调用，`char_move` 端口为空时必须有 fail-safe。建议在 call patch wrapper 里缓存一个 `fn_char_move` 指针？插件不能直接拿；由端口保证。端口在安装期间一定非空，竞态窗口内若为空，wrapper 返回 0（不动）比崩更安全，但会丢一帧移动。记录一次告警。

## 10. 验证矩阵

### 10.1 静态/单元

- 八向分类：N/NE/E/SE/S/SW/W/NW；22.5°/45°/67.5° 边界；死区；中心。
- 主/副方向映射与 `KEY` 索引表。
- 自定义键码翻译：写回 `KEY_nCode`；非自定义码原样；state 清理。
- `CHAR_Move` wrapper：斜向调用顺序、flag、返回值组合；cardinal 单次；flag!=0 透传；非主控/有路径/非 play 透传。
- patch 站点：`0x9d1dc`、原字 `0x9401318B`、解析成功。
- 插件资源安装/卸载/失败回滚。

### 10.2 真机（必须）

| 场景 | 期望 |
|---|---|
| 八个方向触摸 | 对应轴移动；斜向两轴同帧；斜向速度可接受 |
| 中心死区 | 不移动，不抖动 |
| 松开 | 立即停止；`KEY_nCode` 无自定义残留 |
| 斜向撞墙 | 只走可走轴；不穿墙；不触发侧格切图 |
| 斜向遇单位 | 沿可走轴滑开；不斜向推人 |
| 切图点 | 最终格触发；侧格不触发 |
| 拾取 | 正常；动画不加速 |
| 对话/教程/弹窗 | 不移动；关闭后可恢复 |
| 切换角色 | 副轴作用于当前主控 |
| 物理键盘 | 行为不变；不叠加副轴 |
| AI/寻路 | 行为不变 |
| 插件关闭 | 资源还原；恢复原版四向 |
| 不同分辨率/安全区 | 摇杆中心与命中区正确 |

无真机证据前，状态只能是设计稿/未验收。

## 11. 已否决方案

| 方案 | 否决原因 |
|---|---|
| 在自定义键码 handler 里复刻整段 `PressKeyPlay` | 形成第二套移动逻辑，遗漏/漂移风险最大 |
| 每帧调用原 `PressKeyPlay` 两次（主/副键） | 拾取、动画、切图检测、推动都执行两次；侧格事件和动画加速 |
| 全局 hook `CHAR_Move` | 影响 AI/寻路/推动内部调用，需要大量调用者判定 |
| 直接改 `UIPlay_ButtonDirection` 的扇区表 | 只能改变主方向，仍只有一个轴；且原版 4 向视觉/状态机不变 |
| 只用 `0x16` 中心但不覆盖主方向 | 原版主方向按 `0x15` 中心算，主/副轴会不一致 |
| 归一化斜向速度（0.707*delta） | 破坏 8px 对齐，触发 `+0x2e0` 补步；需同时改转向闸门 |

## 12. 实施顺序

1. 加符号、patch 站点、端口契约与 game 层空实现（可编译、单测通过）。
2. 实现八向分类/状态机/键码翻译，单测覆盖。
3. 实现 `CHAR_Move` 两轴 wrapper，单测用假 `fn_char_move` 验证调用序列。
4. 加插件资源与 feature，Debug 构建，设备探针验证 hook/patch 安装与还原。
5. 真机八向验收；修边界（死区、侧格事件、推动、速度）。
6. 再评估全向：先解决 8px 转向闸门，不与八向同批。

## 13. 原型实现记录（2026-10-27）

### 13.1 已落地

- 初版插件 `plugins/eight_way_move/` 当时使用 toggle；当前 feature 为 §0 定义的 choice，稳定 option values 为 `eight_way`/`omnidirectional`。
- 四个私有、必需、fail-closed、可还原资源：
  - `hook:UIPlay_ButtonDirection`（`F_UIPLAY_BUTTON_DIRECTION_VMA`）
  - `hook:GAMESTATE_PressKeyPlay`（`F_GAMESTATE_PRESSKEYPLAY_VMA`）
  - `patch:GAMESTATE_PRESSKEYPLAY_CHAR_MOVE`（`P_GAMESTATE_PRESSKEYPLAY_CHAR_MOVE`，`+0x21c`，原字 `0x9401318B`）
  - `hook:UIPlayPorting_Draw`（`F_UI_PLAY_PORTING_DRAW_VMA`，副轴箭头补画）
- 新窄端口 `QOL_PORT_MOVEMENT_INPUT`（`contracts/game_ports/movement_input_port.h`），仅 `eight_way_move` 可声明。
- 游戏层按职责拆分：
  - `game/input/eight_way_move_math.h`：纯八向分类/键码数学、朝向偏好，可单测。
  - `game/input/eight_way_access.{h,cpp}`：KEY 键码/键表、按住位、屏幕高度、TouchHandle 偏移、控件绝对点、虚拟按键根。
  - `game/input/eight_way_state.{h,cpp}`：触摸状态机、自定义键码、主方向键码翻译、朝向偏好。
  - `game/input/eight_way_move.{h,cpp}`：`CHAR_Move` 双轴 wrapper。
  - `game/input/eight_way_draw.{h,cpp}`：`UIPlayPorting_Draw` 后补画副轴箭头。
  - `game/adapter_ports/movement_input_port.{h,cpp}`：端口实例。
- 插件层：
  - `plugin.cpp`：descriptor、资源声明、生命周期。
  - `feature.{h,cpp}`：端口/门禁、四个 wrapper、backup、诊断日志。
- Host：端口注册与私有端口校验、插件 descriptor 注册。
- 符号/patch：新增 `UIPlay_ButtonDirection`、`GAMESTATE_PressKeyPlay`、`UIPlayPorting_Draw`、`SGL_GetScreenHeight`、`TouchHandle_GetControlOffset`、`ControlObject_GetAbsolutePoint`、`InputLayer_GetVirtualKey`、`ControlObject_FindByID`、`KEY_nCode`、`KEYSET_nData`、`KEY_bPressed`，以及 `P_GAMESTATE_PRESSKEYPLAY_CHAR_MOVE`。
- 入口 hook 符号必须同时登记到 `game/access/symbols.cpp` 的 `kHookEntrySymbols`：`resolve_hook_entry` 只按该白名单解析入口，不读取 `symbol_registry.h` 的完整表。漏登记会让 preflight 报 `symbol unresolved`，插件状态变 `unsupported`，设置页显示「当前版本不支持」——这不是游戏变体限制。
- 按住有效位是 `KEY_bPressed`（`0x7135a0`，dynsym 可解析），不是 `0x3076d3`；`0x3076d3` 实际是 `MAP_bShadowTile1`。读错地址会表现为 `press=0`、所有触摸都被当成取消。
- 摇杆中心必须动态取 `ControlObject_GetAbsolutePoint(ctrl) + 126`。真机逻辑高度为 `640`、显示为横屏 `854x384`，虚拟按键根还有父偏移；固定 `(136,H-126)`/`(200,H-190)` 会让斜向触点被算成上下左右。

### 13.2 验证

- `eight_way_move_tests`：八向分类、22.5°/45° 边界、死区、自定义键码、副轴条件、朝向偏好（右上保持右/上、非轴回退、基本方向忽略），通过。
- `eight_way_move_plugin_tests`：插件 id、4 个资源、端口、feature 默认关闭与 Gameplay 分类，通过。
- `scripts/build-debug.sh`：通过；最终模块产物 `output/inotia4_qol_lsposed_debug_2610270308_9e2037786635.apk`，SHA-256 `9e2037786635cb8007a2bbccc11c88811aa60b14d4e882ad094c14872f8864fd`。
- C18L v586 真机烟测：四个资源均 `live`；日志出现 `eight_way touch`、`char_move hook active`、`eight_way draw secondary=3 loc=0xe at=(212,514)`、`eight_way diagonal applied ... delta=8 step=8`；截图确认上/右双箭头都在方向环上点亮，角色斜向移动。
- 朝向偏好已有单测；多段触摸（先右后右上）真机逐帧验证未完成。
- 完整验收仍 `NOT_ACCEPTED`：松开停止、撞墙、切图/拾取/动画、物理键盘、插件关闭还原和不同分辨率仍需逐项验证。

### 13.3 已知原型取舍

- 两轴使用完整 `delta`（8px → 8px），总速度约 `sqrt(2)` 倍单轴；保持原版 8px 对齐，避免方向切换/松手补步。斜向降速已回退，后续需连同原版 8px 闸门一起处理。
- 主轴 `flag=1`、副轴 `flag=0`；侧格事件被规避，主轴失败时立即停住，不产生只走副轴的侧格位置。
- 主轴成功、副轴被挡时，原型会先反向回滚主轴、再以 `flag=0` 重走主轴，让镜头/格子事件落在单轴终点；回滚失败时直接恢复移动前坐标并返回 2，避免留下静默位移。该回滚分支是原型新增逻辑，需真机重点验证。
- 输入清理用 `owns_input` 区分本功能写入与物理键；同方向物理键与触摸主键完全同码时仍有理论歧义。
- 物理方向键与触摸主方向键码相同时，原型仍靠 `press_active` 和键码匹配判断，理论上存在同方向物理键叠加副轴的边界；真机验证时重点检查。
- 每 30 帧的逻辑帧任务负责离开 Play 状态时清理 state/残留码；不是逐帧强保证。
- 双箭头采用“原版画完后补画副轴”的方式；若原版 HUD 绘制顺序变化，补画可能盖到更高层元素，需真机回归。
- 朝向偏好读取 `lead_member()+C_DIRECTION`；角色在 8px 补步期间可能仍使用上一朝向，属于预期。
- （该项记录于早期原型阶段）全向移动当时未实现；之后已落地 44 项 1px 全向模式。本轮仍沿用该执行/转向闸门，不修改移动实现。
