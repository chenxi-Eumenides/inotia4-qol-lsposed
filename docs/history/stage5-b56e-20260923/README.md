# 阶段 5 批 5-6e：占位路由与 Kotlin 常驻路由归位（VM-K5-06j）

日期：2026-09-23 ｜ 目标：`core.*` 领域 owner 全覆盖——把此前保留 `api.*` 标签的**未实现占位**与**处理器仍在 Kotlin** 的路由归位到所属领域 owner，使 `api.*` 只剩「待迁的真实写路由」。

## 1. 变更（`plugins/core.api/routes.inc`，16 条 + 1 条 owner 字段）

| 路由组 | 旧 owner | 新 owner |
|---|---|---|
| `GET /api/system/events` | api.system | `core.system` |
| `POST /api/op/quest/accept`、`POST /api/op/quest/complete` | api.quest | `core.quest` |
| `POST /api/op/character/{role}/skill-point`、`POST /api/op/character/{role}/skill-level` | api.character | `core.character` |
| `POST /api/op/combat/{role}/heal`、`POST /api/op/combat/{role}/rest`、`POST /api/op/combat/{role}/revive`、`POST /api/op/combat/{role}/hate` | api.character | `core.character` |
| `POST /api/op/inventory/set-slot`、`POST /api/op/inventory/set-equip` | api.item | `core.item` |
| `POST /api/op/craft/mix-direct` | custom_recipe | `core.craft` |
| `GET /api/quest/{id}` | api.quest | `core.quest` |
| `GET /api/ui`、`GET /api/ui/screen`、`GET /api/ui/panel`、`GET /api/ui/dialog` | api.ui | `core.ui` |

仅改 owner 元数据；143 条路径/方法/顺序不变，`op_gated`/`lease_required` 不变。

**文档标注**（api-reference §7.10 新增两小节）：未实现占位路由（501 `not implemented`；`GET /api/quest/{id}` 为 404）+ 处理器仍在 Kotlin 的路由（events 有状态 diff；4 条 ui GET 依赖 `ActivityThread`，JVM-only）。

## 2. 验证

| 项 | 结果 |
|---|---|
| Host CTest | **65/65** |
| `check_arch` / `git diff --check` | 通过 |
| `check_http_routes.py` | 143/143、duplicates 0、双向 missing/extra 0、**exit=0** |
| 构建 | `output/inotia4_qol_lsposed_debug_2609232221_307261b1a114.apk`；LSPatch 3/3；已安装 |
| 真机·归位 | `/api/system/routes` 复核：events→`core.system`、accept→`core.quest`、skill-point→`core.character`、set-slot→`core.item`、mix-direct→`core.craft`、`/api/quest/{id}`→`core.quest`、`/api/ui`→`core.ui` |
| 真机·占位行为不变 | `POST /api/op/quest/accept` → 501、`POST /api/op/inventory/set-slot` → 501、`POST /api/op/craft/mix-direct` → 501、`GET /api/quest/123` → 404 |
| 真机·剩余 | 仅 `api.character` / `api.item` / `api.ui`（39 条待迁真实写） |
| 全量 smoke | `total=143 pass=125 warn=18 fail=0`；与基线 verdict/status diff 仅 4 条已知 party op 环境差异 |
| logcat | FATAL/SIGSEGV/SIGABRT = 0 |

## 3. 过程修正

首版改标脚本的正则 `(,.*)$` 吞掉行尾换行，导致 16 行与下一行合并（路由条数仍 143、检查器仍通过，但文件结构已损）。已用 `git show HEAD:<path> > <path>` 恢复，改用**只替换 owner 文本跨度**的写法重做（保留引号、缩进与 `\r\n`），复核 147 行 / 143 条 / diff 恰 16 行。

## 4. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `plugins/core.api/routes.inc`、`docs/reference/api-reference.md`（§7.10 两小节）、`docs/refactor/api-route-map.md`（计数行）、`docs/refactor/{verification,ledger}.md` | 无扩展背包 R-xx/B-xx | `VM-K5-06j` | 全部已执行：host/arch/routes/构建/LSPatch/真机归位与占位行为/smoke/logcat |

## 5. 剩余

- 待迁真实写 39 条：`api.ui` 5（批 5-7）、`api.item` 13（批 5-8，含修 `/api/character/party/{slot}` 通配遮蔽）、`api.character` 21（批 5-9）。
- 12 条 501 占位（现 owner 已归位）：仅文档标注，不迁。
