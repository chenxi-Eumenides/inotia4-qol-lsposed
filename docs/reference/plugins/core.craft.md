# `core.craft` 插件

## 1. 作用与 owner 定位

`core.craft` 是合成领域的只读 facts provider。它经 `craft.facts` 暴露当前合成界面、配方材料槽及相关角色/物品事实；插件自身不实现合成写操作。

## 2. 功能

### 路由

`routes.inc` 中 `/api/op/craft/mix-direct` 的 owner 是 `core.craft`。API 参考与 route map 将其描述为当前由 Kotlin 占位处理并响应 501；该 owner 元数据不代表 `core.craft` descriptor 提供了相应 native handler。`core.craft` descriptor 不声明 HTTP handler。正式路由和接口说明见 [API 参考](../api-reference.md) 与 [API route map](../../refactor/api-route-map.md)。

### Capability 与 port

- 提供 `craft.facts` v1.0。`contracts/craft_facts.h` 的 API 由两组读取成员组成：`read` 与 `read_only`。
- 必需 Game Adapter port：`QOL_PORT_CRAFT` v1.1；消费者写面 ports 为 `QOL_PORT_CRAFT_UI_WRITE` v1.1 与 `QOL_PORT_CUSTOM_RECIPE_WRITE` v1.1。它们是内部 port ABI。
- `QolCraftReadApi` 与 `QolCraftReadOnlyApi` 中的选中物品、材料格物品、目标槽物品及选中背包物品均以 `QolItemHandle` 返回；空槽使用 `kInvalidItemHandle`，已失效或无法确认 owner 的对象返回明确错误，不伪造有效 handle。
- 填入格控件不跨 `craft.facts` 暴露。`stuff_slot_available(index)` 只返回当前槽位是否可用；`craft_ui` 绘制 hook 收到的原生 control 仅作为同步调用参数交给 Game Adapter 做当前 UIMix 子控件/槽位比较，不缓存其地址，界面关闭或重建后不会复用旧 control 标识。
- Port ABI `QOL_PORT_CRAFT` 为内部 v1.1；`craft.facts` capability 版本仍为 v1.0。`QOL_PORT_CRAFT_UI_WRITE` 与 `QOL_PORT_CUSTOM_RECIPE_WRITE` 为内部 v1.1，物品写操作接收 typed handle；无效 handle 表示清槽，非零 stale handle fail-closed。
- `craft_result` v1.1 的 Place/Execute/MakeItem 输入同样使用 `QolItemHandle`。`custom_recipe` 通过窄的 typed write port 请求产物创建、保存/释放及宝石字段操作；Game Adapter 在同步调用内部解析 handle，插件侧没有 handle→native pointer resolver。物品 handle 保留 registry 的 owner/generation/主线程校验，unknown owner 不按 Game owner 补登记。

### 注册 feature

| feature id | 默认 | 分类 | 类型 |
|---|---:|---|---|
| `core.craft.provider` | 1 | Hidden | toggle |

provider 为常驻需求，用 default true 的 hidden feature 表达；`enable`（Host 独占 lifecycle 期望）与 feature 由 Host 管理，插件不读写。

## 3. 依赖前置与消费者

- descriptor 不声明 capability requires/optional_requires；安装要求 Host SDK 与一个 `QOL_PORT_CRAFT`。
- 实际 capability 消费者为 `craft_ui`、`custom_recipe`、`extension_bag`。`craft_ui`/`custom_recipe` 消费 typed item identity 与槽位事实；`extension_bag` 当前只读取 `read_only.panel_state`，不消费物品或控件身份成员。
- facts 调用通过 Game Adapter port 转接；低层游戏访问不由插件直接实现。

## 4. 职责边界

- 拥有 `craft.facts` 这一只读合成上下文投影及其 provider 生命周期。
- 不拥有合成 recipe/合成操作写命令或 HTTP API 实现；`/api/op/craft/mix-direct` 的 route owner 标注与 Kotlin 501 处理是两个不同事实。
- 不拥有功能配方策略（`custom_recipe`）、合成 UI 与交互（`craft_ui`）、扩展背包数据（`extension_bag`），也不承担 Game Adapter/Host 的低层实现。

## 5. 核心实现链

- **读取**：`plugin.cpp` 为 port 的 read/read_only 每个字段提供 capability 包装函数；port 或成员未注入时返回 not-ready。未创建插件私有快照，也未声明帧任务。
- **线程/缓存**：facts 调用直接委托给注入 port；该插件自身不缓存或异步采样。调用线程纪律由契约消费者及 Game Adapter port 约定。
- **写操作**：没有写命令、HTTP handler 或 ticket result handler；`craft.facts` 是只读 capability。
- **生命周期**：install 校验 SDK、port 数量、ABI 及关键成员；enable/disable 由 descriptor 生命周期回调记录 provider 启停状态；deactivate 清除 port 引用，capability 随插件生命周期发布/撤销。

## 6. 文件地图

| 子职责 | 文件 | 职责 |
|---|---|---|
| provider 实现 | `module/app/src/main/cpp/plugins/core.craft/plugin.cpp` | facts 包装、descriptor、port 校验与生命周期 |
| 插件声明 | `module/app/src/main/cpp/plugins/core.craft/plugin.h` | descriptor 入口声明 |
| capability 契约 | `module/app/src/main/cpp/contracts/craft_facts.h` | `craft.facts` ID、版本及 API 结构 |
| Game Adapter ports | `module/app/src/main/cpp/contracts/game_ports/craft_port.h`、`craft_ui_write_port.h`、`custom_recipe_write_port.h` | typed craft facts、槽位事实与 typed 写操作 |
| 消费者 | `module/app/src/main/cpp/plugins/craft_ui/`、`custom_recipe/`、`extension_bag/` | 合成 UI、自定义配方及扩展背包消费接入；extension_bag 仅消费 panel state |
