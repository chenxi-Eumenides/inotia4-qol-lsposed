# 日志审计批次 1：host + core 插件

按 `logging-standards.md` §1–§7 审计；日志行格式遵循 `logging.md`。范围为 host 与指定 core 插件的生产日志调用。

## 现有日志清单

| file:line | domain | level_now | message 摘要 | event_class | level_target | disposition | rationale |
|---|---|---:|---|---|---:|---|---|

| `host/assembly.cpp:470` | `core` | `W` | `save patch install failed attempt=%u stage=%s claim=%s symbol=%s addr=%p ` | 失败/状态 | `W` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/assembly.cpp:507` | `core` | `I` | `assembly: save patches installed on frame retry attempts=%u` | 失败/状态 | `I` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/assembly.cpp:514` | `core` | `W` | `save patch install retries exhausted attempts=%u last_stage=%s last_error=%s` | 失败/状态 | `W` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/assembly.cpp:532` | `core` | `I` | `device_probe: domain=elf_export symbol=GRPX_Start result=%s expected=hit followed=%s` | 失败/状态 | `I` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/assembly.cpp:536` | `core` | `W` | `device_probe: domain=elf_export symbol=GRPX_Start diagnostic=unexpected_missing` | 失败/状态 | `W` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/assembly.cpp:539` | `core` | `I` | `device_probe: domain=elf_export symbol=QOL_Preflight_Missing_Export result=%s expected=missing followed=%s` | 失败/状态 | `I` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/assembly.cpp:543` | `core` | `W` | `device_probe: domain=elf_export symbol=QOL_Preflight_Missing_Export diagnostic=unexpected_hit` | 失败/状态 | `W` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/assembly.cpp:642` | `core` | `W` | `assembly: save-ready state subscription failed error=%s` | 失败/状态 | `W` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/assembly.cpp:669` | `core` | `W` | `assembly: frame source not ready` | 失败/异常 | `W` | `augment` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/assembly.cpp:738` | `core` | `E` | `assembly: plugin descriptor registration failed id=%s` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/assembly.cpp:746` | `core` | `E` | `assembly: owner store binding failed` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/assembly.cpp:754` | `core` | `E` | `assembly: feature metadata missing for %s reason=%.*s` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/assembly.cpp:764` | `core` | `E` | `assembly: slot operation binding failed` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/assembly.cpp:769` | `core` | `E` | `assembly: save route binding failed` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/assembly.cpp:778` | `core` | `E` | `assembly: platform config owner registration failed` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/assembly.cpp:784` | `core` | `E` | `assembly: security config owner registration failed` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/assembly.cpp:795` | `core` | `E` | `assembly: feature bootstrap failed reason=%.*s` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/assembly.cpp:807` | `core` | `E` | `assembly: feature reconciler bind failed (registry/catalog mismatch or store unavailable)` | 失败/状态 | `W` | `augment` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/assembly.cpp:818` | `core` | `W` | `assembly: management SDK injection unavailable` | 失败/异常 | `W` | `augment` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/assembly.cpp:841` | `core` | `E` | `assembly: event state frame tasks unavailable (fallback=%llu dispatch=%llu)` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/assembly.cpp:871` | `core` | `W` | `assembly: transition dispatch registration failed` | 失败/异常 | `W` | `augment` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/assembly.cpp:877` | `core` | `I` | `assembly: initialized=true registered=%u install_failed=%u kernel_only=%s` | 失败/状态 | `I` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/core_patches.cpp:83` | `core` | `W` | `core_patches: hub site not found` | 失败/异常 | `W` | `augment` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/core_patches.cpp:88` | `core` | `W` | `core_patches: hub install failed: %s` | 失败/异常 | `W` | `augment` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/core_patches.cpp:100` | `core` | `W` | `core_patches: bitend site not found: %s` | 失败/异常 | `W` | `augment` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/core_patches.cpp:105` | `core` | `W` | `core_patches: bitend install failed %s: %s` | 失败/异常 | `W` | `augment` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/core_patches.cpp:145` | `core` | `I` | `core_patches monster compat: 0x%lx cmp w8,#0x62 -> #0x7F` | 失败/状态 | `I` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/core_patches.cpp:149` | `core` | `W` | `core_patches monster compat failed 0x%lx: %s` | 失败/异常 | `W` | `augment` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/core_patches.cpp:157` | `core` | `I` | `core_patches monster compat: no target (non-monster build)` | 生命周期/状态 | `D` | `demote` | 可选对象/资源缺失为正常分支，不应作为 INFO 状态变更。 |
| `host/features/production.cpp:274` | `core` | `E` | `runtime effect observer threw` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/features/reconciler.cpp:62` | `core` | `E` | `feature bind: descriptor mismatch id=%s count=%u descriptor_count=%u` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/features/reconciler.cpp:69` | `core` | `E` | `feature bind: feature id mismatch id=%s` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/features/reconciler.cpp:75` | `core` | `E` | `feature bind: plugin count mismatch ordinary=%u registered=%u` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/features/reconciler.cpp:80` | `core` | `E` | `feature bind: registry prepare failed` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/frame_source.cpp:108` | `core` | `E` | `frame_source: site not found %s` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/frame_source.cpp:113` | `core` | `E` | `frame_source: install failed %s: %s` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/frame_source.cpp:160` | `core` | `E` | `frame_source: claim registration failed %s` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/frame_source.cpp:189` | `core` | `I` | `frame_source installed (render + logic anchors)` | 生命周期/状态 | `I` | `augment` | 成功状态值得 INFO 保留，但须改为结构化 verb+对象字段。 |
| `host/plugin_registry.cpp:908` | `core` | `W` | `optional edge downgraded to avoid cycle: provider=%s consumer=%s ` | 失败/状态 | `W` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/plugin_registry.cpp:1655` | `core` | `E` | `plugin %s capability event handler threw id=%s available=%u` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/plugin_registry.cpp:1674` | `core` | `E` | `plugin %s declares %u provided capabilities; limit is %u` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/plugin_registry.cpp:1704` | `core` | `W` | `plugin %s capability registration failed; retrying on next enable` | 失败/状态 | `W` | `augment` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/plugin_registry.cpp:1900` | `core` | `E` | `plugin %s install unsupported resource=%s: %s` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/plugin_registry.cpp:1910` | `core` | `E` | `plugin %s install failed: plugin install returned nonzero` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/plugin_registry.cpp:2008` | `core` | `W` | `plugin %s resource_ready_failed on %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:2055` | `core` | `W` | `plugin %s resource_ready_failed on %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:2086` | `core` | `W` | `plugin %s install failed: %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:2113` | `core` | `W` | `plugin %s degraded on %s: %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:2195` | `core` | `E` | `lifecycle: deactivate rejected id=%s reason=legacy consumers` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/plugin_registry.cpp:2279` | `core` | `W` | `plugin %s activation skipped: %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:2312` | `core` | `W` | `plugin %s blocked at activation: %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:2323` | `core` | `E` | `plugin %s install unsupported resource=%s: %s` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/plugin_registry.cpp:2345` | `core` | `E` | `plugin %s install failed: plugin install returned nonzero` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/plugin_registry.cpp:2407` | `core` | `E` | `lifecycle: deactivate rejected id=%s reason=legacy consumers` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/plugin_registry.cpp:2421` | `core` | `E` | `feature prepare: missing features id=%s` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/plugin_registry.cpp:2425` | `core` | `E` | `feature prepare: active tx id=%s` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/plugin_registry.cpp:2432` | `core` | `E` | `feature prepare: resource not free id=%s resource=%s state=%d gate=%d` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/plugin_registry.cpp:2440` | `core` | `E` | `feature prepare: status not registered id=%s state=%d` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/plugin_registry.cpp:2459` | `core` | `E` | `plugin %s blocked at assembly: %s` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/plugin_registry.cpp:2702` | `core` | `W` | `plugin %s resource install deferred resource=%s code=%d: %s` | 失败/状态 | `W` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/plugin_registry.cpp:2706` | `core` | `E` | `plugin %s resource install failed resource=%s code=%d: %s` | 失败/状态 | `W` | `demote` | 局部绑定或安装失败不代表模块整体不可用；失败原因字段也需补齐。 |
| `host/plugin_registry.cpp:2751` | `core` | `W` | `plugin %s resource_ready_failed (soft) on %s` | 失败/状态 | `W` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/plugin_registry.cpp:2760` | `core` | `W` | `plugin %s resource_ready_failed on %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:2910` | `core` | `E` | `plugin %s activation failed: %s` | 失败/异常 | `W` | `demote` | 局部绑定、安装或单次回调失败不使整体模块不可用；且正文缺少规范化 reason/code。 |
| `host/plugin_registry.cpp:2955` | `core` | `W` | `plugin %s atomic group deferred: %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:2961` | `core` | `W` | `plugin %s lifecycle failed: %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:2970` | `core` | `W` | `plugin %s lifecycle failed: %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:3124` | `core` | `W` | `plugin %s ownership retry deferred (not ready) on %s` | 失败/状态 | `W` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `host/plugin_registry.cpp:3131` | `core` | `W` | `plugin %s ownership retry failed on %s: %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:3153` | `core` | `W` | `plugin %s ownership ready notify failed on %s` | 失败/异常 | `W` | `keep` | 失败可降级或游戏继续；正文需补 reason/code 及明确对象字段。 |
| `host/plugin_registry.cpp:3156` | `core` | `I` | `plugin %s ownership restored on %s` | 生命周期/状态 | `I` | `keep` | 需按 verb key=value 结构补齐作用域/结果字段，当前自然语言消息不满足正文契约。 |
| `host/ui_adapter.cpp:1472` | `ui` | `W` | `menu replacement check stage=%s a=%lld b=%lld c=%lld d=%lld int=int64` | 失败/状态 | `W` | `keep` | 消息需补齐结构化对象/原因字段以满足正文契约。 |
| `plugins/core.character/plugin.cpp:2127` | `core.character` | `I` | `[core.character] http route table mismatch` | 生命周期/状态 | `I` | `augment` | 需按 verb key=value 结构补齐作用域/结果字段，当前自然语言消息不满足正文契约。 |
| `plugins/core.character/plugin.cpp:2214` | `core.character` | `I` | `[core.character] provider install` | 生命周期/状态 | `I` | `augment` | 成功状态值得 INFO 保留，但须改为结构化 verb+对象字段。 |
| `plugins/core.character/plugin.cpp:2221` | `core.character` | `I` | `[core.character] provider deactivate` | 生命周期/状态 | `I` | `augment` | 需按 verb key=value 结构补齐作用域/结果字段，当前自然语言消息不满足正文契约。 |
| `plugins/core.character/plugin.cpp:2234` | `core.character` | `I` | `[core.character] snapshot unavailable` | 生命周期/状态 | `D` | `demote` | 可选对象/资源缺失为正常分支，不应作为 INFO 状态变更。 |
| `plugins/core.character/plugin.cpp:2245` | `core.character` | `I` | `[core.character] snapshot scheduler unavailable` | 生命周期/状态 | `D` | `demote` | 可选对象/资源缺失为正常分支，不应作为 INFO 状态变更。 |
| `plugins/core.character/plugin.cpp:2248` | `core.character` | `I` | `[core.character] provider enabled: capability + http queries` | 生命周期/状态 | `I` | `augment` | 成功状态值得 INFO 保留，但须改为结构化 verb+对象字段。 |
| `plugins/core.character/plugin.cpp:2254` | `core.character` | `I` | `[core.character] provider disabled: capability + http queries withdrawn` | 生命周期/状态 | `I` | `augment` | 需按 verb key=value 结构补齐作用域/结果字段，当前自然语言消息不满足正文契约。 |
| `plugins/core.craft/plugin.cpp:162` | `core.craft` | `I` | `[core.craft] provider install` | 生命周期/状态 | `I` | `augment` | 成功状态值得 INFO 保留，但须改为结构化 verb+对象字段。 |
| `plugins/core.craft/plugin.cpp:167` | `core.craft` | `I` | `[core.craft] provider deactivate` | 生命周期/状态 | `I` | `augment` | 需按 verb key=value 结构补齐作用域/结果字段，当前自然语言消息不满足正文契约。 |
| `plugins/core.craft/plugin.cpp:172` | `core.craft` | `I` | `[core.craft] provider enabled: capability published` | 生命周期/状态 | `I` | `augment` | 成功状态值得 INFO 保留，但须改为结构化 verb+对象字段。 |
| `plugins/core.craft/plugin.cpp:176` | `core.craft` | `I` | `[core.craft] provider disabled: capability withdrawn` | 生命周期/状态 | `I` | `augment` | 需按 verb key=value 结构补齐作用域/结果字段，当前自然语言消息不满足正文契约。 |
| `plugins/core.quest/plugin.cpp:599` | `core.quest` | `I` | `[core.quest] owner installed: http queries registered` | 生命周期/状态 | `I` | `augment` | 成功状态值得 INFO 保留，但须改为结构化 verb+对象字段。 |
| `plugins/core.quest/plugin.cpp:625` | `core.quest` | `I` | `[core.quest] snapshot unavailable` | 生命周期/状态 | `D` | `demote` | 可选对象/资源缺失为正常分支，不应作为 INFO 状态变更。 |
| `plugins/core.system/plugin.cpp:245` | `core.system` | `I` | `[core.system] provider install` | 生命周期/状态 | `I` | `augment` | 成功状态值得 INFO 保留，但须改为结构化 verb+对象字段。 |
| `plugins/core.system/plugin.cpp:252` | `core.system` | `I` | `[core.system] provider deactivate` | 生命周期/状态 | `I` | `augment` | 需按 verb key=value 结构补齐作用域/结果字段，当前自然语言消息不满足正文契约。 |
| `plugins/core.system/plugin.cpp:259` | `core.system` | `I` | `[core.system] snapshot unavailable` | 生命周期/状态 | `D` | `demote` | 可选对象/资源缺失为正常分支，不应作为 INFO 状态变更。 |
| `plugins/core.system/plugin.cpp:271` | `core.system` | `I` | `[core.system] snapshot scheduler unavailable` | 生命周期/状态 | `D` | `demote` | 可选对象/资源缺失为正常分支，不应作为 INFO 状态变更。 |
| `plugins/core.system/plugin.cpp:274` | `core.system` | `I` | `[core.system] provider enabled: http queries` | 生命周期/状态 | `I` | `augment` | 成功状态值得 INFO 保留，但须改为结构化 verb+对象字段。 |
| `plugins/core.system/plugin.cpp:280` | `core.system` | `I` | `[core.system] provider disabled: http queries withdrawn` | 生命周期/状态 | `I` | `augment` | 需按 verb key=value 结构补齐作用域/结果字段，当前自然语言消息不满足正文契约。 |

## 缺失日志建议

| file + 函数 | 事件 | 目标等级 | 必填字段 | 理由 |
|---|---|---|---|---|
| `module/app/src/main/cpp/host/assembly.cpp` + assembly 初始化成功路径 | 核心 assembly 完成并可用 | I | `op=assemble result=ready` | 当前仅有局部 frame source 成功信息，未记录整体装配完成里程碑。 |
| `module/app/src/main/cpp/host/plugin_registry.cpp` + 成功激活路径 | 插件注册/激活成功状态变更 | I | `op=activate plugin=<id> result=active` | 注册/激活失败有记录，但没有成功状态的可回放 INFO。 |

## 统计

- 现有日志条数：91
- disposition：keep=28、demote=38、promote=0、delete=0、merge=0、augment=25
- 补充建议：2

## Top 发现

1. host 多处把局部 frame-source、plugin、feature 绑定/注册失败记为 E；按规范应为 W，除非明确造成模块整体能力不可用。
2. 多条失败消息只有“failed/not found”，缺少 `reason`/`code`，降低定位能力。
3. provider/install/enable 成功日志使用自然语言句式，缺少规范 `verb key=value` 字段。
4. 多个可选 snapshot/scheduler 缺失作为 INFO 记录，应降为 D；这是正常可选路径缺失。
5. assembly 完成与插件成功激活缺少统一的 INFO 里程碑，无法从日志重建启动状态。

## 审核裁决

仅以下行号修改，其余 keep；原清单中其余 augment 建议不采纳。E→W 与 I→D 均仅改等级；结构化项按裁决修改正文。

- `host/assembly.cpp`: 669, 738, 746, 754, 764, 769, 778, 784, 795, 807, 818, 841, 871
- `host/core_patches.cpp`: 83, 88, 100, 105, 149, 157
- `host/features/production.cpp`: 274
- `host/features/reconciler.cpp`: 62, 69, 75, 80
- `host/frame_source.cpp`: 108, 113, 160, 189
- `host/plugin_registry.cpp`: 1655, 1674, 1704, 1900, 1910, 2195, 2323, 2345, 2407, 2421, 2425, 2432, 2440, 2459, 2706, 2910；新增 INFO 位于成功激活路径
- `plugins/core.character/plugin.cpp`: 2127, 2214, 2221, 2234, 2245, 2248, 2254
- `plugins/core.craft/plugin.cpp`: 162, 167, 172, 176
- `plugins/core.quest/plugin.cpp`: 599, 625
- `plugins/core.system/plugin.cpp`: 245, 252, 259, 271, 274, 280

驳回 assembly 完成里程碑建议：`host/assembly.cpp:877` 已记录初始化状态、注册数与安装失败数。

## 补审记录（清单遗漏）

审计代理只按 `QOL_LOG_*` 宏与 `log_message(` 收集，漏掉直接 `sdk->log.write` 的 snapshot 警告与 `core.quest/feature.cpp` 的 `log_info`。补审结论：

| file:line | 原正文/等级 | 处置 | 结论 |
|---|---|---|---|
| `plugins/core.system/plugin.cpp:79` | `[core.system] snapshot read failed...`，W | augment | 改为 `op=snapshot_read result=failed reason=read_failed_or_capacity keep=previous`；等级 W 不变（失败但保留旧快照，游戏继续）。 |
| `plugins/core.character/plugin.cpp:496` | `[core.character] snapshot read failed...`，W | augment | 改为 `op=snapshot_read result=failed reason=read_failed keep=previous`；W 不变。 |
| `plugins/core.world/plugin.cpp:176` | `[core.world] snapshot read failed...`，W | augment | 同上（core.world 其余日志留给后续批次，本条只统一同族文案）。 |
| `plugins/core.quest/feature.cpp:55` | `quest resync: fixed=%u`，I | augment | 改为 `op=quest_resync result=ok fixed=%u`；resync 次数有限，I 不变。 |

遗漏原因：各插件的本地 `log_*` 封装不经过统一宏，收集时必须同时 grep `log_message(`、`log_info(`、`sdk->log.write(`。
