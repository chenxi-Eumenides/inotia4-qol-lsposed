# ADR 0001：UI/文本域拆分与迁移

状态：已接受（用户裁决，2026-09-24）
适用范围：`core.ui` / `core.ui_state` / `core.settings` / `text_layer` / 角色面板汉化 / `go_main_menu`
前置：阶段 5 批 5-8a（提交 `083404c`）已完成 `core.choice` → `core.ui_state` 与面板写并入。

## 决策

### D1 拆分粒度＝三拆（A）
- `core.custom_ui`：共享组件能力、`ui_panels` 框架、原生选择控件、文本机制
- `core.ui_state`：只持游戏 UI 状态与操作
- `core.settings`：设置成品页 + `own:popup.settings` + 五条设置调试路由
- 旧 `core.ui` **最终撤销**，不保留双 owner

### D2 命名／能力 ID
- 使用架构书已有的三个 `core.*` 名称；`core.` 是官方核心插件命名空间，不是权限概念
- **保留** `ui_components`、`ui_panels`、`ui_choice`、`text_layer` 的能力 ID 与既有版本
- 只迁**提供者**；不让 `world_teleport`、`save_backup` 因插件改名而更换能力契约

### D3 文本分家
- `MEMORYTEXT_GetText` 的唯一 hook、注册与分派 → `core.custom_ui`
- 旧 `text_layer` 不再作为独立产品插件
- 文字**内容不随机制搬入工具插件**：角色面板 → D4 插件；合成特定文案 → `custom_recipe`；设置页标签 → `core.settings`
- `core.craft` 是事实 provider，不接管配方呈现文案

### D4 汉化插件
- 新建 **`character_panel_i18n`**：持有角色面板绘制作用域 hook，并经**保留 ID 的 `text_layer` 能力**注册其属性标签
- 不并入 `core.character`（角色事实），也不并入 `core.ui_state`（游戏 UI 状态/操作）；汉化是可裁剪的呈现功能

### D5 `go_main_menu`
- owner 归 `core.ui_state`；**URL 与 POST 方法不变**
- `core.save` 仍负责进档、建档等存档动作
- 沿用 world／弹窗守卫、扩展背包退出清理、转场单飞；主线程命令**必须走逻辑帧内联路径**，不得在命令里调用会同步等待的 `transition_run`
- 当前 API 文档仍是同步 `{"ok":true,"state":...}`；**若迁为票据，必须先逐条更新响应契约**——不可能只是改 owner 元数据

### D6 绘图 helper＝页面自持（薄 helper）
- 设置页搬走自己用的 helper；`save_backup` 保留现有自持实现
- 低层绘制继续调用已有 `sdk->ui`；**暂不扩大全局 SDK**，也不为约 20 个包装函数升级 `ui_components` 能力
- 待出现稳定的跨页重复需求，再提炼窄共享接口

### D7 配置标签
- **不改** `/api/config/*` 的 `module.config` owner
- 设置页消费与修改配置，但配置自举、存储与恢复**不得依赖设置页插件是否启用**

### D8 elf-export
- **不批量迁**。`hook:<名>` 是 `ResourceClaim.id`，不等于目标符号；资源 ID 保持稳定
- 仅当逐个证实「目标版本存在动态导出 + 函数 ABI + 实际行为」并完成 preflight 与受影响 VM 卡后，才把**该资源**的 symbol 改为 `elf-export:<导出名>`
- `VM-EXPORT-01` 只证明已测目标的机制，不替代功能回归

## 事实校正（已核实）
- `QolUiComponents` 当前为 **14** 个函数指针（`panel_create`、`button_create`、`label_create`、`toggle_create`、`select_create`、`scroll_view_create`、`set_rect`、`button_set_text`、`label_set_text`、`toggle_set_value`、`select_set_selected`、`scroll_set_offset`、`dispatch_touch`、`destroy`）
- `text_layer` 当前表为 **14 条**记录；其中角色面板 12 个 ID（35185–35196）**含 1 个 `nullptr` 空占位**（`charinfo.magic_def` 35188，注释：面板未显示，显式登记以维持区间连续）——迁移清单不得按「12 条非空文案」处理

## 迁移次序（推荐，按批验收）
1. 先让设置页**经能力而非跨插件私有头**使用工具，再迁 `core.settings`
2. 随后逐一转移 `ui_choice` 与全局文本 hook 的**唯一 owner**，拆出文案贡献者（含 `character_panel_i18n`）
3. 最后单独迁 `go_main_menu`
4. D8 随各功能回归**逐目标**执行

## 每批验收要求
- 保留**能力撤销／恢复**、**资源归还**的证据
- 对应真机证据：`VM-D1`、`VM-P4-05`、`VM-45`、`VM-46`、`VM-K5-08a`
- **不能仅凭「路由 143/143」宣告完成**
