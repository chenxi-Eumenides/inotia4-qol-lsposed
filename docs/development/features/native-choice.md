# 原生选择框组件（native-choice）

> 状态：已实现并真机验证（原版选择框恢复可选、世界传送 5 项流程不变）｜位置：`module/app/src/main/cpp/feature/ui/native_choice.{h,cpp}`

## 1. 目标

把"打开原生 UICHOICE 选择框并处理其选中"收敛成**模块唯一**的可复用组件，替代此前各功能自行操作 `G_UICHOICE_*` 全局、挂钩 `UIChoice_ButtonListExe` 的做法。

组件存在的直接原因：世界传送曾**整槽替换**全按钮共用的 ExecuteProc，且替换函数假设"当前框一定是模块自己的"，导致原版选择框（NPC/商店/记忆之门）按下无反应。

## 2. API

```cpp
struct NativeChoiceSpec {
    const char* const* items;   // count 个选项文本；仅需在 native_choice_open 调用瞬间有效
    int   count;                // 1..6（UICHOICE 上限）
    const char* title;          // 可空；非空时作为 choice 主标题
    int   close_index;          // 命中该索引视作"关闭"；-1 = 无关闭项
    void (*on_select)(int index, void* user);  // 仅对 index != close_index 调用
    void* user;
};

bool native_choice_install_if_ready();  // 幂等；bridge_ready() 且 g_base!=0 后调用
bool native_choice_open(const NativeChoiceSpec& spec);
bool native_choice_active();
void native_choice_close();
```

纯逻辑规则（`valid_spec`/`can_open`/`is_close_index`/`address_in_range`）在头文件 `native_choice_detail` 命名空间内，供 host 测试直接使用，不触碰游戏内存。

## 3. 语义与约束

- **原版透传（最重要）**：包装函数在「`!active`」或「active 但 `G_UICHOICE_ITEMTEXT[0]` 指针不在组件文本缓冲区间内」时，**无条件调用原版 ExecuteProc**。前者覆盖"原版任何框"，后者覆盖"标志残留 + 原版复用控件"。
- **接管时**：自行复刻非事件副作用（`SOUNDSYSTEM_Play(2)`、`UI_SetPopupProcessInfo(3,0)`），读 `ControlObject_GetCursorIndex` 写 `G_UICHOICE_FOCUS`；`close_index` 仍**走原版**完整清理链（`UI_SetPopupProcessInfo(3,0)` + `EVTSYSTEM_DoCheckAllEvent(8)`），非关闭项调用 `on_select` 且**不**调用原版（避免误触发 `DoCheckAllEvent(8)`）。
- **文本持久化**：调用方传入的 `items` 只保证调用瞬间有效，组件拷贝进自持缓冲（6×256B）。
- **单活动 + 互斥**：`std::mutex` 串行化；同一时刻仅一个模块框；`open` 失败（已 active、spec 非法、符号不可达、push 失败）返回 false 并保持原状态。
- **标题**：`UIChoice_Init` 包装先调原函数，仅在"模块框激活且有 title"时把 `G_UICHOICE_MAIN_TEXT` 指向组件缓冲。
- **依赖方向**：`feature/world_teleport` → `feature/ui/native_choice` → `game_access` / `game_symbols` / `game_ptr_hook`。

## 4. 集成（世界传送）

`world_teleport.cpp` 只保留业务：构造 5 项文本（0..3「<地图名>(id±N)」、4「关闭」）与标题，调用 `native_choice_open`，回调 `teleport_on_select(index)` 处理 0..3（校验目标 → 关闭 choice → 恢复 HUD gate → 下一逻辑帧创建 YesNo）。入口拦截（`UIPlay_CallMapName` patch）与余额扣费/切图逻辑不变。

## 5. 验证

- Host：`native_choice_tests`（spec 校验、重复 open、关闭索引、地址区间归属）；`ctest` 10/10 通过。
- Android Debug 构建通过（`scripts/build-debug.sh`）。
- 真机：原版选择框恢复可选；世界传送 5 项（±1/±10/关闭）流程与确认框行为不变。

## 6. 决策与理由（思考）

- **为什么用"包装 + 门禁"而不是"自建面板"**：包装改动面最小、原版透传语义可逐字节等价；自建面板（`POPUPSTATE_Create` + `ControlButton_Create` 自挂回调）虽然完全不碰 `UIChoice_ButtonListExe`，但需复刻面板布局/绘制，成本高，保留为需要自定义视觉时的备选实现。
- **为什么必须做归属校验**：`active` 只是模块自己的标志，异常路径（回调抛错、外部关闭）可能残留；`item_text[0]` 落点校验把"模块框"变成**可证伪**的判据，避免误接管原版框。
- **为什么关闭项仍走原版**：原版清理链除关 popup 外还要让 EVTSYSTEM/UIChoice 自己收尾，否则下一帧 `Scene_Process_POPUP_SC_CHOICE` 会访问已失效控件（见 `world-teleport.md` §3.3）。

## 7. 后续可复用的对话框组件候选

同一形态（"唯一持有原版入口 + 门禁透传 + 调用方只传 spec"）可继续覆盖：

| 优先级 | 组件 | 现状重复点 | 建议 API |
|---|---|---|---|
| P0 | `native_popup`（UIPopupMsg 生命周期 + 回调） | `feature/world_teleport/world_teleport.cpp:221`、`feature/extension_bag/extension_bag_equip.inc:1149`、`extension_bag_store.inc:675`、`feature/craft_ui/craft_ui_hooks.cpp:228`、`feature/ui/game_ui_exp.cpp:153` | `native_popup_text/yes_no/close` |
| P0 | `popup_state`（状态表注入/备份/恢复/栈顶校验） | `feature/ui/game_ui_settings_injection.inc:63`、`game_ui_savebackup_injection.inc:3`、`game_ui_autosell.cpp:524`、`game_ui_exp.cpp:190`、`game_ui_custom_injection.inc:3`；已有雏形 `feature/ui/game_ui_kit.h:46-63` | `native_panel_install/open/restore/is_top` |
| P1 | `native_dialog_router`（按栈顶类型路由动作） | `game_dialog_content.inc:22-140` 与 `game_dialog_operations.inc:88-164` 各写一遍同一优先级链 | `native_dialog_snapshot/options/action` |

仅做业务适配器、不建议并入底层组件的：剧情 AVG（EVTSYSTEM 驱动）、NPC 对话（UICHOICE 与 NPCTASKLIST 两套数据模型）、wipeout、任务完成面板；数量输入框独立 Push 无安全路径（`game_ui_operations.inc:107-116`）；Java 同意页不经 libgame，留在 Kotlin 层。
