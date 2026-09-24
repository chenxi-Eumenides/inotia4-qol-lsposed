# 阶段 5 / ADR 0001 第 3c 批：文本机制并入 core.custom_ui；新建 character_panel_i18n（VM-K5-08f）

日期：2026-09-24 ｜ 依据 `docs/refactor/adr/0001-ui-text-domain-split.md` D3/D4。

## 1. 改动

**A. 文本机制 → `core.custom_ui`**
- 新建 `plugins/core.custom_ui/text_mechanism.{h,cpp}`：机制本体（`text_mechanism_bind`、`text_set_active/active`、`text_api_subscribe/unsubscribe`、`memorytext_get_text_wrapper`、`text_on_get_text_ready`、`kResourceIdGetText`），**不含任何文案表**。
- `core.custom_ui/plugin.cpp`：provides **3→4**（+`text_layer`，能力 ID/版本保留）；resources **2→3**（+`hook:MEMORYTEXT_GetText`，shared/kEntryHook/Required/FailClosed）；install `text_mechanism_bind`；deactivate/on_enabled/on_disabled 增 `text_set_active`；`custom_ui_resource_ready` 分派 GetText 分支。CMake 加 `text_mechanism.cpp`。

**B. 新建 `character_panel_i18n`（内容贡献者）**
- `charinfo_scope.{h,cpp}` 自 text_layer 迁入（命名空间 `qol::features::character_panel_i18n`）。
- `feature.{h,cpp}`：12 条角色面板标签表（35185–35196，**35188 显式 `nullptr` 占位**）+ 编译期连续性断言 + `lookup`/`entry_by_key`/`entries` + `owner_table_modify`/`owner_table_lookup`；经**保留 ID** 的 `text_layer` 能力注册（after+Modify）。
- `plugin.{h,cpp}`：requires 1（`text_layer`）、provides 0、resources 1（`hook:Scene_Draw_Popup_SC_CharacterInfo`，private/**FailSoft**）。

**C. 撤销独立 `text_layer` 插件**
- `git rm -r plugins/text_layer`；主 CMakeLists 插件源清单、`host/assembly.cpp` descriptor 注册改为 `character_panel_i18n`。

**D. 配方呈现文案 → `custom_recipe`**
- 新建 `plugins/custom_recipe/text_table.h`（2 条：`recipe.jewel_tier_up` 与 `.title`，均 id `kJewelTierUpLabelWordId`，scope 2/3，文本「宝石升阶」；纯逻辑、host 可测）。
- `custom_recipe/plugin.cpp`：requires **2→3**（+`text_layer`）；install 取能力并 `subscribe(after+Modify)`；deactivate `unsubscribe`；消费者委托 `recipe_owner_lookup`。
- 行为修正：旧实现中该消费者的作用域镜像与 host `active_text_scope()` 不一致，**合成页文案替换实际未生效**；本批改为使用 host 作用域，属对既有惰性的修正（见 §3）。

## 2. 验证

| 项 | 结果 |
|---|---|
| Host CTest | **66/66**（`module_text_tests` 重写覆盖两表：12 条 i18n + 2 条 recipe；`character_panel_i18n_tests` 新增；`native_choice_tests` 期望 provides 4/resources 3） |
| `git diff --check` / `check_arch`（含 check_resources）/ `check_http_routes` | 全过；routes 143/143 |
| 构建 | 普通 `output/inotia4_qol_lsposed_debug_2609241428_ce458ccfdce1.apk`、kernel-only `..._2609241429_4335de3d1741.apk` |
| LSPatch | 3/3，monster v29 已安装 |
| 真机·插件 | `character_panel_i18n` enabled；**`text_layer` 无残留**；`custom_recipe`/`core.custom_ui` enabled |
| 真机·资源归属 | `hook:MEMORYTEXT_GetText` owner=**core.custom_ui**、`live`；`hook:Scene_Draw_Popup_SC_CharacterInfo` owner=**character_panel_i18n**、`live` |
| 真机·撤销/恢复 | i18n deactivate → 其 hook `free`、状态 inactive；activate → `live`/enabled。**core.custom_ui deactivate → GetText `free` 且级联停用 i18n/custom_recipe（installed_disabled）**；按依赖序 activate（core.custom_ui → i18n → custom_recipe）→ 全部 enabled、两资源 `live` |
| smoke | `total=143 pass=125 warn=18 fail=0`，与基线 verdict/status diff 仅 4 条已知 party op 环境差异 |
| logcat | FATAL/SIGSEGV/SIGABRT = 0 |

## 3. 遗留与未闭合

- **角色面板标签与合成页文案的实际显示**需游戏内打开对应界面确认（属深度行为卡范畴，需人工配合）；本批已确保表内容、作用域门控、资源归属与能力接线正确。
- 旧实现在合成页的文案替换**本就不生效**（作用域镜像不一致）；本批改为 host 作用域后会生效，**该行为变化需真机界面确认**。
- 选择控件（3b）与文本钩子（3c）现同属 `core.custom_ui`：停用该插件会同时使两者失效（级联），符合「工具插件」定位。

## 4. 四元组

| 触及文件 | R/B 编号 | VM 卡 | 原因 |
|---|---|---|---|
| `plugins/core.custom_ui/{text_mechanism.*,plugin.cpp,CMakeLists.txt}`、`plugins/character_panel_i18n/*`（新）、`plugins/custom_recipe/{text_table.h,plugin.cpp}`、删除 `plugins/text_layer/*`、`host/assembly.cpp`、`CMakeLists.txt`、`tests/{CMakeLists.txt,test_module_text.cpp,test_character_panel_i18n.cpp,test_native_choice.cpp,test_core_api_lifecycle.cpp}` | 无扩展背包 R-xx/B-xx | `VM-K5-08f` | 已执行：host/静态/双构建/LSPatch/真机（插件、资源归属、撤销恢复、级联、smoke、logcat）；游戏内界面显示待人工配合 |
