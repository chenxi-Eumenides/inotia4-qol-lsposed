# 日志审计清单：批次 1（patch）

依据 `docs/reference/logging-standards.md` 与 `docs/reference/logging.md`。等级按单次 patch/hook 失败后游戏仍可运行判定；只有影响模块整体能力或不可恢复时才用 E。

## 现有日志

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---|---|---|---|---|---|
| `patch/call_patch.cpp:84` | platform | E | patched call table capacity exhausted；limit/call/wrapper | patch 登记容量失败 | W | demote | 单个调用点无法登记，能力可局部降级；不是模块整体不可用。 |
| `patch/call_patch.cpp:107` | platform | E | thunk free list capacity exhausted；slot | thunk 资源回收失败 | W | demote | 空闲槽未能回收影响后续 patch，游戏仍可继续。 |
| `patch/call_patch.cpp:436` | platform | E | thunk allocation failed；call/wrapper/地址/阶段/errno 等 | thunk 分配失败 | W | demote | 单个 patch 安装失败不使模块整体不可用；诊断字段充分。 |
| `patch/call_patch.cpp:449` | platform | E | mprotect failed；addr/errno | 指令写入失败 | W | demote | 本次 patch 写入失败，游戏可继续；失败原因字段应改为 `reason=`。 |
| `patch/call_patch.cpp:563` | platform | E | original word mismatch；call/got/expected | BL patch 安装拒绝 | W | demote | 预期指令不符时 fail-closed 拒绝单个 patch，游戏仍运行。 |
| `patch/call_patch.cpp:599` | platform | D | patched call；call/replacement | BL patch 安装成功 | D | keep | 成功状态可供排查，调用点与替换字均有记录且为 debug。 |
| `patch/call_patch.cpp:611` | platform | W | call patch saved word missing；call | BL patch 还原状态缺失 | W | augment | 定位字段 `call=` 已有；失败正文缺 `reason=`。 |
| `patch/call_patch.cpp:622` | platform | E | revert target is not a BL call；call/current | BL 还原拒绝 | W | demote | 检测到非 BL 后 fail-closed 不覆盖指令，仅此次还原失败。 |
| `patch/call_patch.cpp:629` | platform | D | reverted call；call/original | BL patch 还原成功 | D | keep | 含调用点和恢复字，属诊断细节。 |
| `patch/call_patch.cpp:641` | platform | E | word patch mismatch；addr/got/expected | 指令字 patch 安装拒绝 | W | demote | 指令不匹配时不写入，仅该 patch 不可用。 |
| `patch/call_patch.cpp:647` | platform | D | patched word；addr/new | 指令字 patch 安装成功 | D | keep | 地址和新字足以定位，属 debug 诊断。 |
| `patch/call_patch.cpp:659` | platform | D | reverted word；addr/original | 指令字 patch 还原成功 | D | keep | 地址和恢复字足以定位，属 debug 诊断。 |
| `patch/call_patch.cpp:675` | platform | E | branch original word mismatch；call/got/expected | B patch 安装拒绝 | W | demote | 原字不匹配时拒绝单个 B patch，游戏仍可运行。 |
| `patch/call_patch.cpp:704` | platform | D | patched branch；call/thunk/replacement | B patch 安装成功 | D | keep | patch、thunk 地址可供诊断，且为 debug。 |
| `patch/call_patch.cpp:717` | platform | W | branch patch saved word missing；call | B patch 还原状态缺失 | W | augment | `call=` 定位齐全，但失败缺 `reason=`。 |
| `patch/call_patch.cpp:728` | platform | E | revert target is not a B branch；call/current | B 还原拒绝 | W | demote | 检测到非 B 后 fail-closed，不覆盖指令；影响单次还原。 |
| `patch/call_patch.cpp:735` | platform | D | reverted branch；call/original | B patch 还原成功 | D | keep | 含调用点和恢复字，属 debug 诊断。 |
| `patch/call_patch.cpp:751` | platform | E | branch-return original word mismatch；call/got/expected/alt | 带返回 B patch 安装拒绝 | W | demote | 原字不符则拒绝单个 patch，游戏仍可运行。 |
| `patch/call_patch.cpp:801` | platform | W | branch-return saved word missing；call | 带返回 B patch 还原状态缺失 | W | augment | `call=` 定位齐全，但失败缺 `reason=`。 |
| `patch/save_enter.cpp:30` | save | W | save enter blocked by transition gate；slot | 读档转换门禁拒绝 | W | augment | 单次读档被阻断且游戏可继续；正文应结构化并补 `reason=`。 |
| `patch/save_enter.cpp:41` | save | W | save enter blocked because loading bypass scope is occupied；slot | 读档旁路占用拒绝 | W | augment | 单次读档被阻断；需 `reason=`，并将自然语言整理为键值字段。 |
| `patch/save_enter.cpp:54` | save | W | save enter original failed；slot | 原版读档启动失败 | W | augment | 单次失败且游戏继续，等级正确；正文缺 `reason=`。 |
| `patch/save_enter.cpp:65` | save | W | new game confirmation unavailable; original not invoked | 可选新档确认入口缺失 | D | delete | 静态文本无变量，且该入口为空时直接跳过；没有诊断定位价值。 |
| `patch/save_exit.cpp:24` | save | I | save exit fired | 退出存档状态事件 | I | augment | 生命周期事件保留为 INFO；正文缺 `slot=` 等作用域字段。 |
| `patch/native_hook.cpp:19` | core | E | native_init rejected；reason=invalid_api | native API 绑定失败 | E | keep | hook/unhook API 缺失导致 native hook 能力不可用；已有失败原因。 |
| `patch/native_hook.cpp:24` | core | E | native_init rejected；version/required | native API 版本不兼容 | E | augment | native hook 能力不可用，E 正确；建议以 `reason=api_version_unsupported` 明确失败原因。 |
| `patch/native_hook.cpp:30` | core | I | native_init bound；api_version/hook | native API 绑定成功 | I | keep | 有限生命周期里程碑，包含版本及 hook 指针。 |
| `patch/native_hook.cpp:36` | core | I | native module loaded；name | 游戏 native 模块加载 | I | keep | 模块加载里程碑且带模块名。 |
| `patch/instr_patch.cpp:160` | platform | W | call patch saved word unavailable；addr | B patch 保存原字失败 | W | merge | 与 179、209 是同一失败文案/事实，均有 `addr=`；合并为一个调用点记录，避免同因重复。 |
| `patch/instr_patch.cpp:179` | platform | W | call patch saved word unavailable；addr | 带返回 B patch 保存原字失败 | W | merge | 与 160、209 重复同一保存原字失败事实，定位字段已有。 |
| `patch/instr_patch.cpp:209` | platform | W | call patch saved word unavailable；addr | BL/字 patch 保存原字失败 | W | merge | 与 160、179 重复同一保存原字失败事实，定位字段已有。 |
| `patch/instr_patch.cpp:247` | platform | W | call patch saved word missing；addr | patch 释放时原字缺失 | W | augment | 与 unavailable 不是同一事实；`addr=` 已有，失败正文缺 `reason=`。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `patch/instr_patch.cpp` + `install_claim` | 声明式 patch 安装成功 | I | `hook=`/`patch=`、`result=installed` | 目前底层成功日志只有 `call_patch.cpp` 的 D 级，关闭 debug 后没有可见成功里程碑；安装成功应有 INFO。 |
| `patch/instr_patch.cpp` + `release_claim` | 声明式 patch 还原成功 | I | `patch=`、`result=restored` | 还原成功没有 INFO；有限次数的生命周期状态变化需要可回放。 |

## 统计

- 现有日志：32 条。
- disposition：keep 9，demote 10，promote 0，delete 1，merge 3，augment 9。
- 补充建议：2 条。

## Top 发现

1. `call_patch.cpp` 中 10 条 E 是单次 patch/还原/资源失败；此类失败只使对应 patch 失败且游戏可继续，应降为 W。
2. `instr_patch.cpp:160/179/209` 同为“saved word unavailable”且均有 `addr=`，属于重复事实，合并后保留一个记录点。
3. `save_enter.cpp:54` 单次原版读档启动失败且游戏继续，W 合适，但缺 `reason=`。
4. `save_enter.cpp:65` 是无变量静态文本，且可选入口缺失时只跳过原版调用，应删除。
5. 安装与还原成功目前没有 INFO 里程碑；patch 成功只在底层 D 级记录，建议在 `install_claim`/`release_claim` 增加结构化 INFO。
