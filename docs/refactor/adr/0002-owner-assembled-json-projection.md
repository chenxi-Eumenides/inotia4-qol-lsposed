# ADR 0002：跨域 JSON 状态投影的所有权边界

状态：已接受（用户裁决，2026-09-25）
适用范围：`character.state` capability 及一切跨插件 JSON 响应组装场景（含后续 save/world/static-data 领域的同类需求）

## 裁决原文

> character等全部返回json，拼装由所有者进行，确保相关知识留在相关域中。使用其他域的json时，只进行筛选、重组，不做重拼装。

## 背景

`equip_item`、`unequip_item`、`put_jewel`、`enchant`、`accept_dice` 5 条 item 写路由的成功响应必须逐字嵌入 party/player 状态快照（外部 HTTP 契约冻结，见 `api-reference.md` §背包写面）。这些响应的 owner 是 `core.item`，而状态 schema 属 character 域。两条约束冲突时的候选解法：

- 跨 capability 传 typed POD、由 `core.item` 拼装 JSON——把角色 schema 知识（字段名、顺序、转义、null 语义）复制进物品域，违反"知识不出所有者域"；
- 5 个 case 滞留 `game_system.cpp` 旧 builder——阶段 6 `api/native` 删除批永久受阻；
- owner 域自组装 JSON 投影、跨域只嵌入——被审查判定需要明确例外（T2c 门禁驳回轮）。

## 决策

- **D1 拼装归所有者**：capability 输出的 JSON 一律由所有者域自己序列化。首个实例 `character.state` v1.0：固定 kind（party / player / raw skills）、版本化 buffer、禁通用 router（落地 `032f1146`）。
- **D2 消费边界**：跨域消费者只允许**筛选**（选取字段/子树）与**重组**（容器嵌套、拼入自身响应 envelope），且必须逐字保留所有者 JSON 子树及转义；**禁止**依据他域字段重新拼装或序列化。
- **D3 schema 知识驻留**：序列化知识只允许存在于所有者插件与其测试面；逐字保证由 owner golden（对照 legacy builder 字节相等）+ 写后即时时序断言承担，不再由类型系统承担。
- **D4 先例管制**：后续任何新跨域 JSON capability 一律按本 ADR 备案（capability id、kind 集合、版本、消费者清单），不得默默援引。
- **D5 消费方失效隔离**：消费者以 optional 依赖取用投影，缺失时仅相关 kind fail-closed（在游戏操作执行**之前**拒绝），不得连带停用不依赖该投影的其余路由。

## 每批验收要求

- owner 投影与 legacy builder 的逐字节 golden；写后即时性时序断言；buffer 容量边界；撤销后 `require` 失败；fail-closed 粒度断言。
- 真机卡按批次口径延后至各插件独立测试批（现行全局裁决），延后期间行为卡状态记 `NOT_ACCEPTED`。
- 不能仅凭路由计数或 Host 通过宣告行为收口。

## 关联

- 方案：`.tmp/item-char-writes/plans/T2c-character-state.md`（O1 闭环记录、§1.2/§1.3 契约条款）
- 架构正文：`ARCHITECTURE.md` §5.2 provider 条款（本 ADR 交叉引用）
- 提交：`character.state` v1.0 落地 `032f1146`
