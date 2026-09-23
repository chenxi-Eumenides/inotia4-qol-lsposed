# 阶段 5 批 5-6c：其余领域插件改名为 `core.<domain>`（VM-K5-06h）

日期：2026-09-23 ｜ 依据：用户裁定①（领域插件统一 `core.<domain>`，一个领域一个 owner、读写一体）。
ARCHITECTURE 规则修订已在批 5-6a 完成（§1.2/§5.2/§7.1），本批只做代码与文档同步。

## 1. 改名映射

| 旧插件 | 新插件 | 能力（不变） |
|---|---|---|
| `character_facts` | `core.character` | `character.facts` |
| `item_facts` | `core.item` | `item.facts` |
| `save_facts` | `core.save` | `save.facts` |
| `system_facts` | `core.system` | （无 provides） |
| `craft_facts` | `core.craft` | `craft.facts` |
| `native_choice` | `core.choice` | `ui_choice` |
| `quest` | `core.quest` | （无 provides） |

连同批 5-6a 的 `world_facts` → `core.world`，官方核心插件现为 `core.api`/`core.ui`（共享出口）
+ `core.world`/`core.character`/`core.item`/`core.save`/`core.system`/`core.craft`/`core.choice`/`core.quest`（领域）。

## 2. 改动内容（机械但面广）
- 插件目录（`git mv`）、CMake 目标名（`qol_plugin_core.<domain>`）、C++ 命名空间（`qol::features::core_<domain>`）、
  插件 id 字符串、日志前缀（`[core.<domain>]`）、`plugins/<dir>/` include 路径。
- `host/assembly.cpp`（include + descriptor）、`host/plugin_registry.cpp`（涉及 craft/choice 的引用）、
  `plugins/core.api/routes.inc`（owner：`character_facts`→`core.character` 15、`item_facts`→`core.item` 6、
  `system_facts`→`core.system` 4、`quest`→`core.quest` 5）。
- 测试：`tests/CMakeLists.txt` 源路径、各 `test_*.cpp` 的 include/命名空间/期望 id。
- 文档：`api-route-map.md`（owner 计数行 + native owner 列表）。

**不变**：能力头 `contracts/<domain>_facts.h`、能力 id（`<domain>.facts`）、消费者中的能力局部变量（`g_<domain>_facts`）。

## 3. 过程教训（两点，已修正）
1. **首轮模式漏了不带 `qol::` 前缀的引用**：`host/assembly.cpp` 写的是 `features::<old>::plugin_descriptor`，
   而脚本只替换 `qol::features::<old>` → 6 处 descriptor 引用全部漏改（构建必失败）。补第二轮
   `features::<old>` → `features::core_<new>` 后归零。
2. **脚本不得改写历史证据**：首轮扫 `docs/` 时误改 `docs/history/**`（34 文件）与生成/历史性文档
   （`map.md`、`verification.md`）。已按 `git show HEAD:<path>` 逐文件恢复（DCG 阻断 `git checkout --`）；
   历史记录保持原样，仅 `api-route-map.md`（活映射）随改名更新。

## 4. 验证

| 项 | 结果 |
|---|---|
| Host CTest | **65/65** |
| `check_arch.py` / `check_http_routes.py` | 全部通过 / 143-143、exit=0 |
| `gen_baseline_doc.py` | 已重生成 `docs/refactor/baseline.md` |
| `git diff --check` | 通过 |
| 构建 | `output/inotia4_qol_lsposed_debug_2609232148_a33b20f7be48.apk`；LSPatch 3/3；已安装 |
| 真机·注册面 | `/api/system/routes` 143 条；`core.character` 15、`core.item` 6、`core.system` 4、`core.quest` 5、`core.world` 12；无旧插件名；`/api/system/features` 列出 10 个 `core.*` 插件，旧名 0 |
| 真机·功能 | 进档后 `/api/character/party/count` → `{"count":3}`、`/api/item/inventory/money` → `{"money":28550}`、`/api/world/map/id` → `{"map_id":20,"id_name":"黑暗教团礼堂"}`、`/api/quest/active` → 含中文任务名；`core.system` deactivate → **503** `http owner inactive`、activate → 200 |
| 全量 smoke | `total=143 pass=125 warn=18 fail=0`；与基线 verdict/status diff 仅 4 条已知 party op 门控环境差异 |
| logcat | FATAL/SIGSEGV/SIGABRT = 0 |

## 5. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| 插件目录改名（6 个）+ `host/assembly.cpp` + `host/plugin_registry.cpp` + `plugins/core.api/routes.inc` + `tests/*` + `docs/refactor/{api-route-map,baseline}.md` | 无扩展背包 R-xx/B-xx | `VM-K5-06h` | 全部已执行：host/静态/构建/LSPatch/真机注册面+功能+生命周期/smoke/logcat |

## 6. 遗留（下一批）
- **读写一体尚未完成**：`core.save` 仍只提供 `save.facts`，save 写路由 owner 仍是 `save_backup`（7 条）；
  `core.craft` 同理（craft 写面在 `craft_ui`/`custom_recipe`）。需后续批次把领域写面并入对应 `core.<domain>`。
- ui 5 条（批 5-7，端口 19/20）、item 13（批 5-8）、character 20（批 5-9）；12 条 501 占位仅文档标注。
