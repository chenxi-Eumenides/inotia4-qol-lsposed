# `character_panel` 插件参考

## 摘要

功能插件：把原版人物属性面板（`POPUP_SC_CHARACTER_INFO`）改造成属性总览入口。

- **面板汉化**：把面板战斗属性行标签（游戏 text id `35185..35196`，共 12 个）替换为中文。不提供能力、无 HTTP 路由、不自己挂全局文本钩子、不写游戏内存。
- **属性总览**：占用原版面板的 PopupState（enter 透传 + event 点击检测）；点击面板内容区后延迟一帧经 `ui_panels` 打开自定义面板 `character_panel.overview`，展示 4 个分区、共 61 行属性（8 页，每页最多 10 行）。

插件源码只 include `contracts/` 与自身目录：不 include `game/`/`infra/`/`hooks/`/`patch/`/`host/`，不持有裸 VMA，也不直接调用游戏函数。

除特别说明，文中路径均相对 `module/app/src/main/cpp/`。

## 迁移注意（相对 `character_panel_i18n`）

- 插件 id：`character_panel_i18n` → `character_panel`；命名空间 `qol::features::character_panel`。
- feature id：`character_panel_i18n.translation` → `character_panel.i18n`；新增 `character_panel.overview`。
- 旧配置对象 `character_panel_i18n` 不再对应任何已注册插件（Host 按未注册数据原样保留，不参与 lifecycle）；旧 `character_panel_i18n.translation` 的开/关值不会被新插件读取。新插件首次注册时按 descriptor 默认值补齐（两个 feature 默认开启）。
- 面板 id `character_panel.overview` 为新增；无旧 id 兼容面。

## 定位与版本

`character_panel`，插件版本 `0.1.0`（`plugins/character_panel/plugin.cpp`）。descriptor：

- `requires_count=3`：`text_layer` v1.0、`ui_components` v2.23、`ui_panels` v1.3；`optional_count=0`。
- `provides_count=0`（纯消费者）；`resources_count=2`；`jni_count=0`。
- `port_count=1`：`QOL_PORT_CHARACTER_EXTRAS` v1.2（`ctx.ports[0]`）。
- 无 `routes.inc`、无 `http_queries`、无快照/事件/命令端口。

注册于 `host/assembly.cpp`（include 与 `plugin_descriptor` 登记）。

## 公开能力与 feature

不 `provides` 能力、不注册 HTTP 路由、无对外快照/事件/命令端口。两个用户可见 feature（均 `kToggle`、默认 `1`、分类 `kBasicOptimization`）：

| feature | 名称 / 描述 | 门禁作用 |
|---|---|---|
| `character_panel.i18n` | 属性面板汉化 / 人物属性显示为中文 | `on_toggle` 立即写 `set_translation_enabled(enabled)`：关闭后 `owner_table_modify` 一律返回 `nullptr`，即使文本消费者仍在册也不替换 |
| `character_panel.overview` | 属性总览 / 点击人物属性面板内容区打开属性总览 | `on_toggle` 写 `set_overview_enabled(enabled)`：关闭后点击不再置 pending，provider `open()` 与 `block_reason` 也会拒绝 |

门禁是**运行时**开关：不提前注册/撤销资源，资源生命周期仍由插件 enable/disable 驱动（ARCHITECTURE.md §8.2）。当前分类与默认值登记见 `docs/reference/plugins/settings.md`。

## 游戏内行为

### 资源

两条声明式资源，均 `kPrivate` / `kRequired` / `kFailSoft`、`resumable=true`、`step_budget=120`、`gate=nullptr`（常驻）、`owner=character_panel`：

| 资源 id / 符号 | 机制 | 职责 |
|---|---|---|
| `hook:Scene_Draw_Popup_SC_CharacterInfo` / `F_SCENE_DRAW_POPUP_SC_CHARACTER_INFO_VMA` | `kEntryHook` | 面板每帧绘制入口 wrapper：进入时 push `kCharacterPanel` 作用域 → 转发原绘制 → 退出时恢复上一层作用域并 pop |
| `own:popup.character_info` / `own:popup.character_info` | `kOwnership` | `acquire`：`popup_resolve("F_PANEL_CHARACTER_INFO_ENTER")` → `popup_acquire`。wrap **enter（只透传原函数）+ event**；process/f3/f4 传 `nullptr`，宿主保留原函数。`release`：`popup_restore` |

`enter` 在机制层是占用身份（owner token）：`hooks/popup_state.cpp` 要求 `hooks.enter != nullptr`，否则 acquire 直接失败；因此 `character_info_enter_wrapper` 必须非空，且内部调用 `entry.backup + kPopupStateEntryEnterOffset` 取出的原 enter。`F_PANEL_CHARACTER_INFO_ENTER` 在 `game/access/symbols.cpp` 登记为可按名解析的导出名；插件不持有裸 VMA。

### 汉化标签表

12 条记录覆盖 `35185..35196` 连续升序，`scope` 全为 `kCharacterPanel`：

| key | text id | 文案 | 说明 |
|---|---|---|---|
| `charinfo.dmg` | 35185 | 物攻 | DMG |
| `charinfo.magic_dmg` | 35186 | 法攻 | M. DMG |
| `charinfo.def` | 35187 | 防御力 | DEF |
| `charinfo.magic_def` | 35188 | `nullptr` | 面板未显示，显式占位以维持区间连续、保持原文 |
| `charinfo.crit_rate` | 35189 | 暴击率 | |
| `charinfo.hit_rate` | 35190 | 命中率 | |
| `charinfo.crit_dmg` | 35191 | 爆伤 | |
| `charinfo.phys_res` | 35192 | 物抗 | P.RES = 物理抗性 |
| `charinfo.magic_res` | 35193 | 法抗 | |
| `charinfo.evade` | 35194 | 闪避率 | |
| `charinfo.weapon_block` | 35195 | 武器格挡 | |
| `charinfo.shield_block` | 35196 | 盾牌格挡 | |

编译期 `static_assert(charinfo_block_ok(), ...)` 保证条目数 == 区间长度、逐项升序无空洞、scope 全为 1。`35181..35184`（LV/EXP/HP/MP）保持游戏原文。表本身归本插件；文本机制（`MEMORYTEXT_GetText` 唯一 hook、注册与分派）归 `core.custom_ui` 的 `text_layer` 能力。

### 点击链路

1. `character_info_event_wrapper(event,param,param2)`（`charinfo_watch.cpp`）：
   - `0x17`（按下）：用 `ui->touch_coordinates` 记录按下点，并判定是否在 `ui->character_info_content_rect` 给出的内容区（已含 inset）内；
   - `0x18`（抬起）：要求**按点与抬点都在同一内容区**内、且 `character_panel.overview` 开启，才置 `g_pending_open=true`；
   - `0x19`（移动）：清除按下点，后续抬起不再触发。
2. 命中判定纯函数 `character_info_tap_inside(rect,x,y)`：左上包含、右下不包含；空/负尺寸恒不命中。
3. **三类事件始终调用原函数并返回原返回值**（无论是否命中、开关是否开启），关闭 X、属性 +/−、队伍页签等原生控件语义不变，也不会被吞掉。
4. 常驻帧任务 `character_info_open_tick`（`kQolRuntimeFramePointLogicPre`，interval=1，count=0）消费 pending，经 `ui_panels` 能力的 `open("character_panel.overview")` 打开面板；event 内不同步 open。打开失败只记一条 warn（`character overview open rejected`），不重试。
5. 帧任务 handler 正常路径**返回 1**（scheduler 语义：返回 0 即注销）；只有 deactivate 显式 `scheduler.remove`。install 与 activate/`on_enabled` 都走幂等注册，deactivate 清空句柄以便重新激活时重注册。

### 属性总览面板

`overview.cpp` 向 `sdk->panels.register_provider` 注册 `PanelProviderSpec{panel_id="character_panel.overview", owner="character_panel", open, close, active, block_reason, callbacks}`，callbacks 非空（enter/process/f3/f4/event）。PopupState 占用、入栈、`set_process_info`、出栈与 popup 还原由 `core.custom_ui` 的 PopupHost 托管。

- 打开条件：`ui_components` 能力就绪且 `character_panel.overview` 开启；`block_reason` 返回 `ui components unavailable` / `character overview disabled`。
- 布局：根面板 rect 为 `{inset_x, inset_y, 960, 640}`（屏幕 inset 原点 + 设计区尺寸），所有子控件与文本一律使用 960×640 **设计区局部坐标**（标题中心 480、左列 90、值列中心 520、箭头位于 480±120）；导航盒由屏幕绝对坐标换算为根内局部坐标（`kNavBoxAbs - inset`），因此横屏/平板黑边不会把按钮或文本挤出可视区。
- 页面：`panel_create` 根 + 标准导航盒（`ui_theme::kNavBox*`）+ 左右箭头按钮 + `draw_control_text` 文本；关闭走导航按钮 → `ui_panels_request_back`，插件不直接 `provider.close()`、不调 `set_process_info`。
- 翻页：`arrow_button_set_enabled` 按边界置灰（第一页禁用左、最后一页禁用右），点击回调只改 `g_page` 并重绘，**不重建控件树**。
- 原版人物属性面板在总览打开期间不绘制、不接收事件，属预期行为。

#### 分页结构（61 行 → 8 页）

每页最多 10 行；分区超过 10 行时按顺序拆到后续页（"后续页顺延"），因此实际是 8 页而不是 4 页。分区行数：主属性与面板属性 16、隐藏属性 22、元素与抗性 19、全局加成 4。

| 页 | 分区 | 本页行数 | 行内容 |
|---|---|---|---|
| 1 | 主属性与面板属性 | 10 | 力量、敏捷、体力、智力、精力、物攻、法攻、防御力、暴击率、命中率 |
| 2 | 主属性与面板属性 | 6 | 爆伤、物抗、法抗、闪避率、武器格挡、盾牌格挡 |
| 3 | 隐藏属性 | 10 | HP吸收(千分比)、固定吸血、治疗增幅、双倍攻击概率、重击概率、增伤阶段、命中嘲讽、反伤、移动速度、武器攻击帧 |
| 4 | 隐藏属性 | 10 | 仇恨系数、嘲讽概率、蓝耗增减、命中回蓝、命中固定回蓝、额外普攻开关、额外普攻增幅、命中沉默、反击减益、HP上限 |
| 5 | 隐藏属性 | 2 | MP上限、MP恢复 |
| 6 | 元素与抗性 | 10 | 火、风、寒气、神圣、黑暗、毒、重力摆、冰霜、治愈气息、狂战士 |
| 7 | 元素与抗性 | 9 | 瞬间恢复、魔力专家、减少敌意值、失明抗性、睡眠抗性、眩晕抗性、恐惧抗性、沉默抗性、减速抗性 |
| 8 | 全局加成 | 4 | 掉落率、经验加成、金币获得、幸运（纯说明行，无数值） |

隐藏分区已删除「蓝耗减免(83)」重复行；`attr 83` 以装备词缀表 `ITEMOPTINFOBASE` 的编码为权威记为「魔力专家」（元素分区），`81` 为「狂战士」。该 id 在旧逆向注释与词缀表之间存在命名冲突，**冲突本身尚未真机确认**。

行表、分页映射与格式化是纯逻辑，位于 `plugins/character_panel/overview_model.h`；`overview.cpp` 只负责读取与绘制。全局加成每行都带注记「不是角色属性（佣兵团技能 / 掉落表 / 怪物表）」。

#### 数据来源

角色指针：`QolCharacterExtrasPort::read.party_member(0,&ch)`，失败回退 `menu_character(&ch)`；两者都失败时所有数值行显示 `--`。

| 行 | 来源 |
|---|---|
| 力量..精力 | `QolCharacterReadApi::main_stat(ch, 0..4, &v)` |
| 物攻 / 法攻 | `normal_damage` / `normal_magic_damage` |
| 防御力 | `attr(17)` 与 `attr(18)` 组合：`attr17*(attr18+1000)/1000`（int64 计算） |
| 暴击率 / 爆伤 / 法抗 / 武器格挡 / 盾牌格挡 | `attr(0)` / `attr(3)` / `attr(11)` / `attr(19)` / `attr(20)` |
| 命中率 / 闪避率 / 物抗 | `hit_rate_1000` / `evasion_rate_1000` / `display_damage_reduce_rate` |
| 隐藏属性 | `attr(id)` 原值：33、36、42、52、66、71、72、76、91、93、94、98、32、63、112、106、109、111、110、31、29 |
| HP 上限 | `QolCharacterExtrasPort::read.max_hp(ch,&v)`；**不回退** `CHAR_GetAttr(30)` |
| MP 上限 | `attr(31)` 原值（宿主暂无对应 getter；代码内有注释说明） |
| 元素与抗性 | `attr(id)` 原值：22..27、78..90 |
| 全局加成 | 不读取游戏数据 |

`attr 30` 不能走 `CHAR_GetAttr`：它在 HP > maxHP 时会把 HP 夹回上限并写回角色，读属性产生副作用。`max_hp` 取不到时该行显示 `--`，不做 attr 回退。

#### 数值格式

- 千分比类（暴击率、命中率、爆伤、物抗、法抗、闪避率、武器格挡、盾牌格挡）：`raw/10`，保留 1 位小数 + `%`；如 `1234 → "123.4%"`、`0 → "0.0%"`、`-5 → "-0.5%"`。
- 其它数值：整数（隐藏属性按 attr 原值显示，`33` 的标签已注明千分比口径）。
- 全部 `snprintf` 写入固定缓冲（数值 40B、说明行 128B、页码 48B），无动态分配。

#### 数据读取约束

- 只在**游戏主线程**、面板 `enter` 内读取一次；`process` 只格式化与绘制，不逐帧读游戏内存。
- 每一行独立记录 `ok`；任一读取失败**只让该行显示 `--`**，不会整体打开失败。
- 宿主只读面在非游戏线程返回 `kQolErrBlocked`（如 `hit_rate_1000`/`evasion_rate_1000`/`display_damage_reduce_rate`）；本插件只在 enter（主线程）调用，因此正常路径不会命中该错误，若被误用则同样落到 `--`。
- 折叠/重开（enter 再次进入）会重新读取一次并把页码复位到第 1 页。

## 已知约束

`ui_components` 契约目前没有帧封装 API，而 `draw_control_text`/箭头/导航绘制必须在 GRPX 帧内执行。因此 `process` 仍调用 `sdk->ui->begin_frame()/end_frame()`，并用 `screen_inset_width/height` 取布局原点、`delete_control` 释放面板根 native 控件。除此之外**不调用任何低层绘制原语**：没有 `fill_rect` / `draw_string_with_font` / `draw_image_part` / 手绘图形，所有可见内容都经 `ui_components`。若后续给 `ui_components` 增加帧封装能力，这两处适配层调用可以移除。

## 失败语义（fail-soft）

- 两条资源均 `kFailSoft`：安装失败记 `degraded`，不使插件整体失败；ownership 由宿主按重试语义定向恢复。
- `install`：SDK/端口校验失败、`ui_components` require 失败、`register_consumers` 失败、`register_provider` 失败 → 返回 1（宿主回滚并把插件置 `failed`）。帧任务注册失败只记 warn（`overview frame task registration failed`），汉化与面板 provider 仍可用。
- `on_resource_ready`：入口 hook id 交 `charinfo_scope_on_resource_ready`（空 backup 返回非 0 → 资源撤回）；ownership id（含启动期 `backup=nullptr` 恢复）返回 0。
- `scene_draw_charinfo_wrapper`：`text_scope.push` 不可用或失败时只透传原绘制、不标记作用域。
- 点击链路：坐标/矩形任一失败按未命中处理，仍调用原 event；`ui_panels.open` 被拒只记 warn。
- 属性读取：单行失败显示 `--`；角色指针取不到时全部数值行为 `--`；宿主只读面在非游戏线程返回 `kQolErrBlocked`，该行同样显示 `--`。
- `fill_status` 输出 `enabled = !注册失败 && host_sdk()!=nullptr`，失败时 `error="text consumer registration failed"`。

## 生命周期

- `install`：绑定 SDK → 校验端口 → require `ui_components` → 绑定 Game Host/端口与组件表 → 注册文本消费者 → 注册面板 provider → 幂等注册常驻打开帧任务。
- `on_enabled`：`set_translation_enabled(true)` + 幂等 `register_consumers()` + 幂等 `ensure_open_task()`（deactivate 后再 activate 能重新注册）。
- `on_disabled`：关文本门禁并注销消费者；面板与资源由 Host lifecycle 流程回收。
- `deactivate`：关文本门禁 → 注销消费者 → `ui_panels.deactivate_owner(character_panel)` → `panels.unregister_owner` → 移除帧任务（无论 remove 结果都清空句柄，避免陈旧 id 阻止重注册）→ 清 pending/按下点 → 关闭面板控件 → 解绑 Game Host/组件表/SDK。
- 资源归还（含 PopupState 条目还原）由宿主资源事务执行；`character_info_reset()` 只丢弃 pending 与按下点，保留原始 enter/event 入口直到 ownership `release`，避免还原失败时 wrapper 失去原函数。测试可用 `character_info_test_reset()` 强制清理（生产路径不调用）。
- 禁用期间的 entry hook 由 Loader 保留为空转 wrapper（`resumable=true`）。

## 文件地图

| 文件 | 事实位置 |
|---|---|
| `plugins/character_panel/plugin.cpp` | 版本、两条资源、requires/ports、provider 声明、install/deactivate/on_enabled/on_disabled/on_resource_ready/fill_status、两个 feature、descriptor。 |
| `plugins/character_panel/plugin.h` | 插件声明面注释与 `plugin_descriptor()`。 |
| `plugins/character_panel/feature.h` / `feature.cpp` | 12 条标签表、区间 `static_assert`、查表、SDK 绑定、`owner_table_modify`、消费者注册/注销、汉化门禁。 |
| `plugins/character_panel/charinfo_scope.h` / `.cpp` | 绘制窗口作用域 wrapper（push→转发→pop）与 backup 交付。 |
| `plugins/character_panel/charinfo_watch.h` / `.cpp` | PopupState 所有权（enter 透传 + event 点击检测）、`character_info_tap_inside`、按下点/`pending` 状态、常驻帧任务 `character_info_open_tick`、测试用 `character_info_test_reset`。 |
| `plugins/character_panel/overview_model.h` | 分区行表、`kTotalRowCount=61`、`pages_in_section`/`total_page_count`/`locate_page`、`format_value`（纯逻辑，host 可测）。 |
| `plugins/character_panel/overview.h` / `.cpp` | provider（open/close/active/block_reason）、callbacks、数据读取（enter 一次）、控件创建与分页绘制、`set_overview_enabled`。 |
| `plugins/character_panel/CMakeLists.txt` | 静态库 target（feature/plugin/charinfo_scope/charinfo_watch/overview）。 |
| `host/assembly.cpp` | include 与插件注册。 |
| `game/access/symbols.cpp` | `F_PANEL_CHARACTER_INFO_ENTER` 按名解析登记。 |
| `contracts/text_layer.h`、`contracts/ui.h`、`contracts/panels.h`、`contracts/ownership.h`、`contracts/game_ports/character_extras_port.h`、`contracts/game_host.h` | 能力/组件/面板/所有权/端口/只读面契约。 |
| `tests/test_character_panel.cpp` | host 测试：descriptor（requires/ports/resources/features）、标签表与作用域、属性总览行模型/分页/格式化/命中判定。 |

## 验证口径

Host/构建：

- `./scripts/build-debug.sh` 通过（唯一构建入口）。
- `character_panel_tests` 通过，覆盖：
  - descriptor：requires（text_layer/ui_components/ui_panels）、ports（CHARACTER_EXTRAS 1.2）、resources 含 `own:popup.character_info`、两个 feature id 与默认值；
  - 汉化：标签表连续性/35188 占位、backup 交付与作用域 push/pop、注册失败可观测；
  - 属性总览纯逻辑：行数/分页（61 行 → 8 页、边界页首尾）、千分比/整数/`--` 格式化、内容区命中（含边界）；
  - PopupState 装配：acquire 传入的 hooks `enter`/`event` 非空（`process/f3/f4` 为空 = 部分 wrap）、enter wrapper 转发原 enter、重复 acquire 幂等、release 调 `popup_restore` 并清空缓存入口；
  - 点击流程：按下/抬起同区打开一次、按点在外不打开、`0x19` 清除按下点后不打开、开关关闭不置 pending、event 始终返回原值、tick 连续两次返回非 0（scheduler keep）且只打开一次。
- `module_text_tests` 通过（两张文本表完整性）。
- `ui_adapter_tests` 通过：`character_info_content_rect` 的 inset 投影、`nullptr` 与 inset 不可用错误语义。
- `host_adapter_tests` 存在与本功能无关的**预存链接缺口**（HEAD 起 `item_inventory_state_write_port()` 等目标源未链接；工作区另有未完成的 `world_*` 依赖）。本功能新增的 `QolCharacterReadApi` 字段与符号注册表断言**仅编译通过，未在该 target 运行**。

真机待验收（Host 测试不能替代）：

1. 点击原版面板内容区命中（含屏幕 inset 与边界），空白区/原生控件（X、属性 +/−、队伍页签）行为不变；按下点在外、抬起在内不触发。
2. 总览打开/关闭链路：点击 → 下一帧打开 → 导航按钮返回 → 回到原版面板；关闭期间原版面板不绘制、不接收事件。
3. 8 页翻页与首尾箭头置灰；翻页不重建控件。
4. 队伍角色与菜单角色两条取值路径、各 attr id 数值、防御公式、千分比格式化与单行 `--` 降级；HP 上限走 `max_hp`、MP 上限直读 attr31。
5. ownership 资源 live/释放、插件停用后 entry hook 与 PopupState 条目归还、`overview` 开关关闭后不再打开。
6. 汉化文案与总览面板共存时互不影响（作用域只在绘制窗口内）。
7. 快速「关闭面板后立刻再点」时，pending 已被上一帧消费/丢弃的行为符合预期（不出现延迟误开）。
