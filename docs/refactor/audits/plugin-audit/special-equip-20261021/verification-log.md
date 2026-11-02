# `special_equip` 插件审阅登记（2026-10-21）

- 审阅对象：功能插件 `special_equip`（`module/app/src/main/cpp/plugins/special_equip/`，7 文件 650 行）。
- 现场基线：HEAD `5b9b20f8`；工作区 11 个与本插件无关的未提交改动（autosell / save_backup / core.item / extension_bag 等），本批未触碰。
- 修复提交：`ec90f718`（审阅缺陷）、`bf487f69`（参考说明）、`50707436`（设计优化批）。
- 阶段 1 由 @oracle 按 A–G 七维完成，共 12 条 finding（P1 2 / P2 7 / P3 3）；阶段 2 裁决后全部静态修复。设备面未验收。

## §1 本批登记问题

Findings 与处置以本节为准；开放/设备欠账同步登记 `docs/refactor/issues.md` §15。

| 编号 | 级别 | 发现 | 处置 / 状态 |
|---|---|---|---|
| SE-01 | P1 | 装备重放 ① 用 `QolItemInventoryView.equip`（`item_is_equip` = 类别记录 `+6` bit0 数量未编码），不是原版 `ITEM_IsRealEquip`。反汇编确证两者读不同字段，存在放行非真装备穿戴的路径。 | **已修** `ec90f718`：改用 `item.facts.is_real_equip(handle, out)`，与原版同函数。静态确证；真机行为未验收。 |
| SE-02 | P2 | 装备需求等级用 `view.need_level`（`ITEM_GetAbilityLevel`，只读类别 `+3`），原版 ④ 调用 `ITEM_GetEquipLevel`（含 chaos 加成、负值归零、上限裁剪）。 | **已修** `ec90f718`：改用 `game->read_only.item.equip_level`。静态确证；chaos/裁剪物品的真机表现未验收。 |
| SE-03 | P2 | 出售门禁只看 `desc_type` 与菜单角色，不区分物品；特殊 NPC 装备详情中的普通装备也会被拦，超出 VM-P4-06「专属位装备不可出售」的字面范围。 | **用户裁决：保持拦全部装备详情**，不改代码；VM-P4-06 增加口径注记。真机目视未做。 |
| SE-04 | P2 | 出售谓词 `set_intercept` 登记失败只 WARN，插件仍 enabled，漏拦无状态可见。 | **已修** `ec90f718`：登记失败记 ERROR、清运行时全局并返回非 0，插件安装失败。 |
| SE-05 | P3 | `desc_type` 仅把 `<0` 视为未知，未知正值（如 3）走 fail-open。 | **已修** `ec90f718`：只接受 `0/1/2`，其余拦截；补 `desc_type=3` 用例。 |
| SE-06 | P2 | 两个装备 wrapper 不检查 `g_gate`；资源部分安装或释放失败隔离时残留 wrapper 仍会覆写原版。 | **已修** `ec90f718`：原版拒绝后检查 `gate_enabled()`，关闭即透传。 |
| SE-07 | P1 | hook 释放失败进入 quarantine 后 Host 仍调用 `plugin_deactivate`，该回调清空 `g_character_extras`；残留 `can_unequip_wrapper` 直接解引用 port，空指针崩溃。 | **已修** `ec90f718`：wrapper 补 port/方法指针空值守卫；入口增加 `InFlightGuard`。释放失败注入未做。 |
| SE-08 | P2 | 注释声称 HTTP 线程可安全进入；item handle 注册/查询仅主线程，事实指针为普通全局，wrapper 无在途保护。 | **已修** `ec90f718`：注释改为事实表述；wrapper 增加 `InFlightGuard`。当前仓库未发现非主线程调用点，未做跨线程实测。 |
| SE-09 | P3 | `capabilities.require == nullptr` 与 `deps_ready()` 失败分支未就地清理已写入的全局。 | **已修** `ec90f718`：统一 `clear_install_globals()`，所有 install 失败出口调用。 |
| SE-10 | P2 | `character.facts` 仅用于 `log_reject` 诊断，却是 required 硬依赖。 | **已修** `ec90f718`：requirements 只保留 `item.facts`；`character.facts` 移入 optional_requires，缺失仅 WARN。 |
| SE-11 | P2 | 源码称「常开，无配置开关」，descriptor 是可持久化关闭的 Hidden toggle；Debug 关闭会触发生命周期卸载。 | **已修** `ec90f718`（仅注释）：保留 Hidden toggle 作为 Debug 禁用口，Release 不显示；行为不变。 |
| SE-12 | P3 | `test_special_equip_rules.cpp:51-58` 注释与断言相反（「一律放行」「放行（不误拦）」），断言本身是 fail-closed。 | **已修** `ec90f718`：修正注释，补未知 `desc_type` 用例。 |

## §2 无法验证项

用户决定（2026-10-21）：本批不进行真机验证。以下设备项全部保持 **NOT_ACCEPTED**，此后不再主动验收；仅保留静态与 Host/构建证据。

以下均为 **NOT_ACCEPTED**，没有真机行为证据，不得写为 PASS：

| 项目 | 无法验证内容 | 结果/限制 |
|---|---|---|
| VM-P4-06 行为面 | 普通队友/主角穿脱 26 条专属位装备；特殊 NPC 穿脱限制；出售门禁；`hook:CHAR_CanEquipItem` / `hook:CHAR_CanUnequipItem` 两态 | **NOT_ACCEPTED**：本批未部署、未执行真机卡。2026-09-17 的用户确认早于 SE-01/SE-02 原语替换，不能覆盖当前实现。 |
| SE-01 可达性 | 「数量未编码但非真装备」的实际物品类别与穿戴后果 | 反汇编确证判据不同，但未枚举目标静态表中的实际差异组合，也未真机触发。 |
| SE-02 等级差异 | chaos 加成、负值归零、上限裁剪物品上 `AbilityLevel` 与 `EquipLevel` 的实际差值 | 反汇编确证函数语义不同；未在真机特定装备上对照。 |
| SE-03 出售范围 | 特殊 NPC 装备详情中普通装备被拦、背包/扩展袋放行 | 用户裁决保持现范围；无真机目视结论。 |
| SE-07 释放失败路径 | hook 释放失败 → quarantine → `plugin_deactivate` → 残留 wrapper 调用 | 静态守卫已加；未做 unhook 失败注入或真机复现。 |
| SE-06 部分安装窗口 | 两个 hook 分帧安装期间单侧生效、关闭/释放失败后的透传 | 静态门禁已加；未做安装失败注入。 |
| SE-08 跨线程路径 | 非主线程经 `CHAR_CanEquipItem` 进入的 fail-closed 行为 | 仓库内未找到调用点；未实测。 |
| 构建产物部署 | 本批 Debug APK 的设备身份与运行日志 | 未安装；模块 SHA 与真机证据未取得。 |

## §3 已完成验收登记

本节只登记静态、Host 与构建证据，不代表真机行为通过。

| 验收项 | 结果 | 证据 |
|---|---|---|
| 阶段 1 静态审计 A–G | **完成** | 12 条 finding；P0 0、P1 2、P2 7、P3 3。 |
| 原语反汇编取证 | **完成** | monster v29 `libgame.so` MD5 `4c332e9b508b35fe1e97f1f26e56f04d`；`ITEM_IsRealEquip` `0x105ab8`、`ITEM_GetAbilityLevel` `0x1091f4`、`ITEM_GetEquipLevel` `0x1097ac`、`CHAR_CanEquipItem` `0xe4eb4`。见 `evidence/disassembly-primitives.md`。 |
| Host CTest（全量） | **PASS（149/149）** | `cd module/app/src/main/cpp/tests && cmake -B build && cmake --build build && ctest --test-dir build`；`special_equip_rules_tests` 含 `desc_type=3` 用例；`special_equip_feature_tests` 编译 `feature.cpp`/`plugin.cpp`/`rules.cpp` 并覆盖 descriptor、安装、wrapper、出售门禁、生命周期与诊断。 |
| 架构检查 | **PASS** | `uv run python scripts/maintenance/check_arch.py`：0 违规。 |
| HTTP 路由检查 | **PASS（147/147）** | `uv run python scripts/maintenance/check_http_routes.py`：plugin 140 + resident 7；duplicates 0、missing/extra 0。无路由变更。 |
| 资源安装点检查 | **PASS** | `uv run python scripts/maintenance/check_resources.py`：违规 0；常驻资源重复安装 0。 |
| Debug 构建 | **PASS** | `scripts/build-debug.sh`：BUILD SUCCESSFUL；APK `output/inotia4_qol_lsposed_debug_2610210404_aa0ab50266d5.apk`；SHA-256 `aa0ab50266d5ba27b5cdf2bf345b832b8b2d3ae8e6b937d34d735d296d0254b3`。首轮构建 `156cf3c832d8…` 对应 `ec90f718`；设计优化批后重建为当前 APK。构建成功不代表已部署或行为验收。 |
| `git diff --check` | **PASS** | 两批提交前均通过。 |

## §4 阶段 2 用户裁决

| 议题 | 裁决 |
|---|---|
| SE-01 真装备判定 | 先核对两条原语是否等价，再选更符合原版的一条。反汇编确证不等价后，采用 `ITEM_IsRealEquip`。 |
| SE-02 需求等级 | 先核对哪条路径给出真实装备等级；确证原版 ④ 使用 `ITEM_GetEquipLevel` 后采用 `equip_level`。 |
| SE-03 出售拦截范围 | 保持拦截特殊 NPC 装备详情中的全部装备，不按 bit4 收窄。 |
| SE-04 门禁登记失败 | 登记失败即插件安装失败，不静默降级。 |
| SE-10 `character.facts` | 改为 optional。 |
| SE-11 常开语义 | 保留 Hidden toggle 作为 Debug 禁用口，修正源码/文档表述。 |

## §5 参考说明核对

`docs/reference/plugins/special_equip.md` 已按最终实现建立并加入 `docs/INDEX.md`。阶段 0.75 首次生成后发现并修正 4 处事实漂移：`view.equip` 不等于 `ITEM_IsRealEquip`、`need_level` 不等于 `ITEM_GetEquipLevel`、出售门禁不区分物品、wrapper 跨线程声明。修复后文档已同步原语替换、optional 依赖与 install 失败语义。

## §6 基础设施观察（不并入插件缺陷）

- `hooks/dispatch` 布尔门固定容量 8；同 ID 同函数幂等，异函数或满槽拒绝。
- `clear_intercept` 只按 resource id 清除，不校验登记者；并发 clear 后已快照的回调仍可能执行一次。
- `on_resource_ready` 逐资源回调；Host 不会替插件拦截分帧安装窗口，需插件自身门禁与在途保护。
- item-handle「lookup 失败 → register」回退模式在 `custom_recipe`、`core.item` 同样存在，均受主线程限制。

## §7 设计审阅批（P1+P2+P3，`50707436`）

用户要求在审阅后评估设计优化空间。结论：保持 `special_equip` 为独立叶子插件，不并入 `core.item` / inventory hook；三文件切分合理；`InFlightGuard`、`g_gate` 镜像、Hidden toggle、安装全有或全无保持现状。

| 项 | 实施 | 结果 |
|---|---|---|
| P1 wrapper host 测试 | 新增 `special_equip_feature_tests`，编译 `feature.cpp`/`plugin.cpp`/`rules.cpp`，fake Host SDK / ItemFacts / CharacterFacts / CHARACTER_EXTRAS / Game Host | 覆盖 descriptor 与资源绑定、install 成功/失败、optional 依赖、两个 wrapper 的透传/gate/重放/fail-closed、出售门禁、资源回调与 deactivate、debug 诊断早退。Host 149/149 |
| P2 规则输入结构 | `EquipReplayInputs` / `SellGateInputs`；wrapper 读取一次，同一快照交给规则与诊断 | 消除位置参数误传与重复读取；`rules` 测试同步 |
| P2 诊断收敛 | 删除诊断用 `item.facts.view`，改用 `category`；`log_reject` 先判 debug 再耗预算/读事实 | 依赖面缩小，debug 关闭时拒绝路径零事实读取 |
| P3 清理 | 删除未使用的 `log_debug` 与 `g_override_count`；保留不带计数的 debug 放行日志 | 减少热路径原子操作与死代码 |
| 保持现状 | 不做 `State` 原子快照、wrapper 模板化、新增注入接口 | 避免为抽象而抽象 |

未做：日志 domain `17` 的契约化上移（单插件不值得扩展基础设施）；拒绝明细保留，作为 UI 灰化唯一诊断路径。
