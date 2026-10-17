# `stack_limit` 插件审计登记（2026-10-17）

> 范围：`stack_limit` 插件、Host 常驻 `item_count` facade/codec 与相关验收口径。静态审计由 @oracle 按 A–G 七维完成，共 8 条 finding（SL-01..SL-08），无 P0/P1。Host/静态证据与真机行为验收分别登记；不以 Host 或构建结果替代真机结论。
>
> 修复提交：`644d27f6`（bitend ×2 从 `stack_limit` 原子组迁至 Host 常驻；插件资源组为 16 条）、`e307fdce`（facade 单次模式快照）、`6d05cddf`（codec 边界矩阵）、`54c0f8f9`（参考说明）。

## §1 本批登记问题

Findings、处置与当前状态以 [`docs/refactor/issues.md`](../../../issues.md) 正文 stack_limit 项及附录为台账：

| 编号 | 发现与处置 | 台账/状态 |
|---|---|---|
| SL-01/SL-02 | P2，已裁决：`item_count` 保持为 Host 常驻窄适配面；本批文档任务同步修订架构文字。该项是边界裁决，不代表验收通过。 | `issues.md` §12；裁决已登记 |
| SL-03 | facade 每次操作曾读取两次模式快照；已由 `e307fdce` 修复并补 Host 覆盖。 | `issues.md` 附录；Host 覆盖，真机不适用 |
| SL-04 | P2，关闭时 `kOff` 早于资源物理撤销的过渡窗口，仍需真机验证。 | `issues.md` §12；开放，NOT_ACCEPTED |
| SL-05 | P3，`g_enabled` 与 Host status 在停用期可能分叉；并入 ENG-04 处置。 | `issues.md` §12/ENG-04；开放 |
| SL-06 | 测试曾未覆盖真实 16 条 descriptor 与 codec 边界；已补对应测试，行为验收仍欠真机证据。 | `issues.md` 附录；真机行为未验收 |
| SL-07 | P2，验收卡/规则册存在 6 条资源、插件拥有 S2、`stack_codec_impl.h`、重定向“常驻”等旧口径；同批文档任务修订。 | `issues.md` §12；当前文档口径已修订，历史结论不改写 |
| SL-08 | P3，旧审计提及的 `g_provider` 裸指针已不存在，当前为静态 `const kFacade`；`INF-05` 仅登记源码已变化，待用户确认销账。 | `issues.md` §12/INF-05；未宣告关闭 |

另：`SL-BITEND` 的修复与机制范围见 `issues.md` 附录；bitend ×2 已迁至 Host 常驻，机制仅为袋槽容量掩码，不代表全部物品子项。

## §2 无法验证项

以下均为 **NOT_ACCEPTED**，没有真机行为证据，不得写为 PASS：

| 项目 | 无法验证内容 | 结果/限制 |
|---|---|---|
| VM-P4-08 | S2 大堆叠出售/拆堆、monster 读档、hub 跳过、999 clamp 两态。 | **NOT_ACCEPTED**：本批未执行真机行为验收。 |
| VM-P1-S2 | canonical >99 的开/关 A/B；`stack_limit` 原子组失败注入。 | **NOT_ACCEPTED**：缺少 >99 真机载体与失败注入证据；既有 EB 逻辑袋视图证据不覆盖这些项目。 |
| VM-P6-01 | 真机 residency 与开关双态（含 deactivate）。 | **NOT_ACCEPTED**：未取得本批设备运行日志。 |
| INF-01 | 关闭后 `slot corrupt` 的触发链。 | **NOT_ACCEPTED**：当前未证实该触发链；monster 校验已由常驻 compat 放行，bitend 仅处理袋槽容量掩码，不能据此认定完整存档触发链已闭合。 |

## §3 已完成验收登记

本节只登记 Host 与静态/构建检查；真机项不在此列为通过。

| 验收项 | 结果 | 证据 |
|---|---|---|
| Host CTest（全量） | **PASS（145/145）** | 本批 Host CTest 实际结果：145/145 通过。 |
| 架构检查 | **PASS** | `uv run python scripts/maintenance/check_arch.py`：通过。 |
| HTTP 路由检查 | **PASS** | `uv run python scripts/maintenance/check_http_routes.py`：通过。 |
| Debug 构建 | **PASS** | `scripts/build-debug.sh` 成功；APK `output/inotia4_qol_lsposed_debug_2610171550_0358e6554805.apk`；SHA-256 `0358e655480529b74763a427db9ec48244dea4c2d303eadb789357d116ff7cb8`。构建成功不代表已部署或行为验收。 |
| `git diff --check` | **PASS** | `git diff --check`：通过。 |
