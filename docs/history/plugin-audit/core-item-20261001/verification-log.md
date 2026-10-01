# core.item 插件审计批（2026-10-01）验收登记

> 本登记记录本批验收范围与限制；不表示 core.item 全量验收通过。完整问题定义以 `docs/refactor/issues.md` 为准，验收矩阵以 `docs/refactor/verification.md` 为准。
> 提交链：`1088967b`（FIX-10 EB draw-end 边沿修复）、`31de93f9`（FIX-11 character attrs/stats 非整数 400）、`4d77ef22`（ITM-06/07 数字校验语义与 Kotlin 传输层去重复校验）、`11710579`（verification 文档）。

## §1 本批登记问题

问题正文与状态均见 [`docs/refactor/issues.md`](../../../refactor/issues.md)，本节只登记编号与本批关联：

- core.item：ITM-01～ITM-11；其中 ITM-10 已按用户裁决接受现状，ITM-11 并入 ENG-04，不在本插件单独修复。
- 本批修复关联：FIX-10、FIX-11；修复提交分别为 `1088967b`、`31de93f9`。FIX-11 属 character 行为，真机非整数专项仍按未完成范围处理。
- ITM-06/07 数字字段语义：缺失字段仍为原 `... required`；字段存在但非有效 int32（小数、指数、前导 `+`、溢出或超长）返回 400 `bad value for <field>`；非法 JSON/非对象返回 `bad request`。`InventoryActionController` 与 `ShopController` 曾使用 org.json `optInt` 预校验（例如 `move_item` 的 `bag>10`），将 `1e2` 归一化为 100 并提前返回聚合 required，遮蔽 native 语义；现按 `4d77ef22` 去除 Kotlin 重复业务校验并原样透传，由 native 作为唯一校验源。
- 新登记：ITM-15（扩展背包设置页开关无效；用户报告），EB 相关 item 验收因此暂不通过。传输层同型余留见 ENG-09，待后续按插件处理。
- BackupMeta 观察项：导出中的 `hero_level`/`class_idx` 是由导出时已加载槽 hero 对象派生的运行时值，非明文重解析。该项仅作观察记录，不据此宣告数据损坏，也不扩展为已结论。

## §2 无法验证项

以下事项由用户确认留到后续其它插件审阅时再测；此后不再主动验收。同步登记于 `issues.md` §10 的 UNVER-02。

| ID | 项 | 原因与范围 |
|---|---|---|
| UNVER-02a | 掷骰 | 当前没有 pending dice。 |
| UNVER-02b | 镶嵌 | 当前背包没有宝石。 |
| UNVER-02c | 强化 | 没有原版强化卷轴，且 EB 关闭。 |
| UNVER-02d | 商店购买 | 商店为空。 |
| UNVER-02e | ITM-01 `bag5` 任务袋读取 | 当前任务袋为空，不能验证有任务物品时的读面。 |
| UNVER-02f | `bag6 slot11` 写入前态 | EB 关闭时无法读取该槽位，故未取得写前态。 |

此外，设置页 EB 开关无效是本批发现的验收阻塞项（ITM-15），不是上述存档内容缺失类项目；EB 相关 item 验收暂不通过。用户报告后留待后续插件审阅处理。

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| FIX-10 EB draw-end 边沿修复 | 提交链已登记；详见对应 FIX 条目。此登记不扩大其人工物理触摸卡的验收范围。 | 提交 `1088967b`；`docs/refactor/issues.md` 附录 FIX-10；`docs/refactor/verification.md` §5.8 |
| FIX-11 character attrs/stats 非整数守卫 | 修复提交已登记；character 真机非整数专项不由本表宣告通过。 | 提交 `31de93f9`；`docs/refactor/issues.md` 附录 FIX-11 |
| ITM-06/07 item 数字守卫（VM-K5-08h） | **PASS（item 侧专项）**：28 项守卫请求全部 HTTP 400 且不产生票据。 | `evidence/guard-matrix.json`（逐项请求、响应及无 ticket 确认）；汇总 `evidence/verification-summary.json` |
| Item 写回归 | **PASS（限定本批 6 项）**：6 项写回归 ticket 均 `done`。 | 本批源记录路径及取舍说明见 `evidence/README.md`；原始大记录未整体迁入以控制体积。 |
| 保护备份恢复 | **PASS**：恢复后 money、bag0 与 party 均与写前一致；备份导入成功。 | `evidence/verification-summary.json`、`evidence/restored-money.json`、`evidence/restored-inventory.json`、`evidence/restored-party.json` |
| ITM-10 白名单重放 | 用户裁决**接受现状不动**，作为已知行为，不作为本批待修项。 | `docs/refactor/issues.md` ITM-10；消费者及行为见 §1 |
| ITM-11 status 语义 | 并入 ENG-04 全局处理，不在 core.item 单修。 | `docs/refactor/issues.md` ITM-11 / ENG-04 |
| core.item 审计批整体 | **NOT_ACCEPTED**：存在 §2 未验证项，且 ITM-15 使 EB 相关 item 验收暂不通过。上述 item 守卫 PASS 不等于插件全量验收通过。 | 本节各专项结果；`docs/refactor/issues.md` UNVER-02、ITM-15 |

证据目录仅迁入守卫矩阵、结果汇总、代表性票据和恢复后状态；未复制完整写回归大 JSON、logcat 或 `.qol_save` 保护备份。源目录和取舍说明见 `evidence/README.md`。
