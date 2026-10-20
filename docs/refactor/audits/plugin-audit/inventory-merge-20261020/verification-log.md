# `core.item` 库存域合并验收登记（2026-10-20）

- 合并对象：`inventory_hooks` + `move_merge` → `core.item`。
- 提交：`73cf0918`（inventory_hooks）、`6cc08cde`（move_merge）。
- 现场基线：设备 `192.168.3.56:5555`，API `8088`，`monster v29`；模块安装前 `core.item`/`inventory_hooks`/`move_merge` 均 enabled。
- 备份：`20261020-194919_s0_c4bb60d4489c.qol_save`，SHA-256 `ae7506db…`（`.tmp/inventory-merge/backup/`，未入库）。
- 证据目录：`evidence/`（health/features/config/screen/inventory/sha256/module-identity）。

## §1 本批登记问题与裁决

| 编号 | 内容 | 处置 |
|---|---|---|
| MERGE-D1 | 两个插件都并入 `core.item`，分两提交 | 已执行 |
| MERGE-D2 | 24 条库存 hook 保持一组 `kRequired + kFailClosed` | 已执行；缺符号时 `core.item` 整体失败 |
| MERGE-D3 | `MOVE_MERGE_GOT` 改 `kOptional + kFailClosed`，`wrapper_ready` 门控 `move_merge.control.enabled()` | 已执行 |
| MERGE-D4 | `world.facts` 改 optional，出售调用时 require；缺失返回 `kSymbolsUnresolved`，无副作用 | 已执行 |
| MERGE-D5 | 删除 `inventory_hooks.inventory_support`；库存 wrapper 常驻 | 已执行 |
| MERGE-D6 | `drag_merge` 关闭时 wrapper 整透传原版；不再保留 EB projection 路由 | 已执行；EB 退化按用户口径接受 |
| MERGE-D7 | 配置迁移保留旧 `move_merge` 有效态（`enable && feature`） | 已执行；设备旧命名空间已删除，`core.item.features.move_merge.drag_merge=true` |

门禁修复：

- P1-1：`hooks/transaction.cpp` 的 `fail_install_resource()` 改为仅 `kRequired + kFailClosed` 硬失败；其余软失败并武装常驻资源重试（ownership 仍由宿主 SaveEnter 路径单独处理）。新增 tx 级 optional 成功/required 回滚用例。
- P1-2：旧顶层 `move_merge.enable` 读取与有效态计算修正；仅写成功后删除已知旧键，保留未知字段；补 `enable=false` 等迁移用例。
- P1-3：`core.item` 参考说明改写为合并后形态；`inventory_hooks`/`move_merge` 参考页改为并入说明；`docs/INDEX.md` 移除独立链接；settings/extension_bag/autosell/ARCHITECTURE 同步。

遗留（P2/P3，已登记 `issues.md` §14）：

- 真实 24 条 claims 仍只有编译覆盖；`item_facts_plugin_tests` 使用 stub claims。
- `core.item` 的 optional `inventory.merge_projection` 失败会把基础 provider 标 degraded（item.facts 实际可用）。
- 拖拽合并的人工目视、EB 投影拖放、S2 999 交互未在本批真机执行。
- `test_core_feature_exposure` 的 host 分类优先级用例为改动前既有失败（`production.cpp` 未改）。

## §2 无法验证项

- **拖拽合并真实 UI 行为**：未执行人工拖拽；仅验证 wrapper 资源 live、feature 开关与 host 行为用例。无真机画面证据，不宣称 VM-43/VM-D2D 行为通过。
- **扩展背包投影拖放/合并**：未在 module view 内做人工拖放；EB 状态 API 正常（`enabled=true`、`injected=true`），但投影 drop 行为未验。
- **999 堆叠与合并上限**：未做 S2 999 下的拖拽合并真机测试；仅 host 用例覆盖 99/999 边界。
- **其他变体**：仅 `monster v29` 真机；original/overhaul/c18 未部署。
- **完整 smoke/写副作用**：未跑全量 smoke；仅只读 API 与 feature 开关。
- **真实 claims 运行时断言**：见 MERGE-01。

## §3 已完成验收登记

| 验收项 | 结果 | 证据 |
|---|---|---|
| Host 全量 CTest | **PASS 147/147** | `module/app/src/main/cpp/tests/build`；含 `hooks_transaction_tests`、`item_facts_plugin_tests`、`move_merge_plugin_tests`、`core_settings_config_store_tests` |
| 架构/路由 | **PASS**：check_arch 全绿；routes 147/147，core.item 21，duplicates/missing/extra 0 | `check_arch.py`、`check_http_routes.py` |
| Debug 构建 | **PASS**：APK `output/inotia4_qol_lsposed_debug_2610201949_41aea6270081.apk`，SHA-256 `41aea627…` | `scripts/build-debug.sh` |
| 集成包 | **PASS**：`output/Inotia4_v1.3.2_monster_v29_lspatched.apk`，SHA-256 `d8e93524…` | `scripts/patch-apk.sh` |
| 设备模块形态 | **PASS**：模块 sha256 `41aea627…` 与 Debug APK 一致；`core.item enabled/degraded=false`；**25/25 资源 live**；provides 6 项；`inventory_hooks`/`move_merge` 独立插件不再出现 | `evidence/features.json`、`evidence/module-identity.txt` |
| 配置迁移 | **PASS**：旧 `move_merge`/`inventory_hooks` namespace 消失；`core.item.features.move_merge.drag_merge=true` | `evidence/config.json` |
| `drag_merge` 开关 | **PASS（静态/状态面）**：false→true 持久化成功；`core.item` 保持 enabled、25 资源 live、无重启/卸载 | `/api/config/set`、`/api/config/receipt/{1,2}` |
| 物品读面 | **PASS**：`GET /api/item/inventory` 返回真实背包；`core.item` 能力面可用 | `evidence/inventory.json` |
| EB/autosell | **PASS（状态面）**：均 enabled、degraded=false；EB 22 资源 live | `evidence/features.json` |
| 备份恢复路径 | **已取证**：备份导出成功并拉回校验；未执行恢复 | `evidence/sha256.txt` |
