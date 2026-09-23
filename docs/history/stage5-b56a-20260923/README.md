# 阶段 5 批 5-6a：世界域统一为 `core.world`（读写一体）+ 修复 world GET 状态透传（VM-K5-06f）

日期：2026-09-23 ｜ 依据用户裁定：领域插件统一命名 `core.<domain>`、一个领域一个 owner、读写一体。

## 1. 改动

- **ARCHITECTURE 规则先行**（3 处）：
  - §1.2 树与说明：`core.*` 定义为**官方核心插件命名空间**（共享出口 `core.api`/`core.ui` + 领域 owner `core.<domain>`，一个领域一个 owner、读写一体、内部再分读面/写面），保留「不是高权限层」「不得用单个 `core.game` 聚合全部领域语义」两条禁令。
  - §5.2：官方核心插件命名为 `core.<domain>`，与共享出口同族，按领域分类、读写一体。
  - §7.1 冻结规则新增**调试端点豁免**：`/api/debug/*` 属开发调试用途、不构成对外调用方契约，其响应可随内部机制调整（如票据化），须在 api-reference 标注为调试端点。
- **改名**：`plugins/world_facts` → `plugins/core.world`；构建目标 `qol_plugin_core.world`；C++ 命名空间 `qol::features::world_facts` → `qol::features::core_world`；插件 id 字符串 `world_facts` → `core.world`；`host/assembly.cpp` include 与 descriptor 引用、`tests/CMakeLists.txt` 源路径、两个 world 测试的 include/命名空间/期望 id 同步。
- **路由 owner**：`routes.inc` 的 7 条 `/api/world/map/*` GET owner `world_facts` → `core.world`（143 不变）。
- **修复既有缺陷**：`InfoMapQueries`（批次 4-1 的 7 条 world GET 的 Kotlin 包装）此前**只读信封 body、忽略状态**，导致 owner 停用时返回 `200 + {"ok":false,"error":"http owner inactive"}`；改用统一的 `NativeHttpQuery.get`（状态感知，≥400 转 `ApiException`），与 item/quest/system/character 一致。

## 2. 验证

| 项 | 结果 |
|---|---|
| Host CTest | 65/65（改名后无回归） |
| `check_arch` / `check_http_routes` | 全过；routes 143/143、exit=0；`gen_baseline_doc.py` 已重生成 `docs/refactor/baseline.md` |
| 构建 / LSPatch | 普通 `output/inotia4_qol_lsposed_debug_2609232123_5a644cc55fdf.apk`；3/3 |
| 真机·注册 | `/api/system/routes` 中 `owner=core.world` 7 条、无 `world_facts` 残留；`/api/system/features` 列出插件 id `core.world` = `enabled` |
| 真机·功能 | 进档后 `/api/world/map/id` → `{"map_id":20,"id_name":"黑暗教团礼堂"}`（含静态注入） |
| 真机·生命周期 | deactivate `core.world` → `/api/world/map/id` → **503** `{"ok":false,"error":"http owner inactive"}`；activate → 200 恢复 |
| 真机·级联（既有语义） | 停用 `core.world`（提供 `world.facts`）会**级联停用**其消费者（quest、world_teleport、extension_bag 等）；重新启用提供者不自动拉起消费者，需显式 activate。EB 一次 `failed: install failed` 后显式 activate 即恢复 `enabled`。此为批 4 已登记的级联设计，非本批引入 |
| smoke | `total=143 pass=121 warn=22 fail=0`，与基线 verdict/status **diff=0**；`FATAL/SIGSEGV/SIGABRT=0` |

## 3. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `ARCHITECTURE.md`、`plugins/core.world/*`（原 world_facts）、`host/assembly.cpp`、`plugins/core.api/routes.inc`、`tests/CMakeLists.txt`、`tests/test_world_facts_{plugin,dispatch}.cpp`、`InfoMapQueries.kt`、`docs/refactor/{api-route-map,baseline}.md` | 无扩展背包 R-xx/B-xx | `VM-K5-06f` | 已执行：host/静态/构建/LSPatch/真机（注册·功能·生命周期·级联）/smoke/logcat |

## 4. 遗留与后续

- 世界域**写面**（5 条，现属 `world_teleport`）尚未并入 `core.world`——读写一体的后半步，下一批执行。
- 其余领域按同一模式推进：`character_facts`→`core.character`、`item_facts`→`core.item`、`save_facts`+save 写→`core.save`、`craft_facts`→`core.craft`、`system_facts`→`core.system`、`quest`→`core.quest`、`native_choice`→`core.choice`。
- 能力 id（`world.facts` 等）暂保留不改。
