# world_tuning 等级上限（`level_cap`）验证记录（2026-11-18）

验收口径见 `../../verification.md` VM-WT-01。本批为新增子项，设备面未执行。

## 范围

- `CONSTBASE` 行 0（角色/佣兵等级上限，s16）读写收口到 `QOL_PORT_CHARACTER_EXTRAS` v1.4 的 `rules.level_cap_get/set`；游戏访问层为 `game/access/level_cap.cpp`（GOT `0x2f5b78` 双层解引用）。
- `world_tuning` 新增配置项 `level_cap`（5..255，默认 105）：面板基础档位 105..255 步长 5，OP 扩展档位 5..255 步长 5；`char_add_experience_wrapper` 调用原函数前校正表值，停用时写回 105。
- 模块侧等级读取统一为 u8 零扩展（`ch+0xe` 由 `strb` 写、`ldrb` 读）：`game/access/save.cpp`、`game/access/craft.cpp`、`game_system.cpp`、`game/ops/character_state_read.cpp`、`game/access/world_read.cpp`、`host/legacy_json_builders.h`、`plugins/save_backup/bundle.cpp`；职业、槽位号等字段保持 int8。
- 测试对齐：`test_world_tuning_feature.cpp` 的面板能力事件断言改为 `panel_available()` 口径（provider 已改为装配期声明，旧的 `register_provider` 计数断言自 `873556ae` 起失效）。

## Host 验证

命令：

```
cd module/app/src/main/cpp/tests
cmake --build build --target world_tuning_rates_tests world_tuning_feature_tests insn_patch_tests item_facts_plugin_tests character_facts_plugin_tests world_legacy_readers_tests -j8
ctest --test-dir build --output-on-failure -R '^(world_tuning_rates_tests|world_tuning_feature_tests|insn_patch_tests|item_facts_plugin_tests|character_facts_plugin_tests|world_legacy_readers_tests)$'
```

结果：`100% tests passed out of 6`。

静态预检：

```
uv run python scripts/maintenance/check_arch.py --only check_layers --only check_offsets --only check_jni --only check_resources --only check_symbols --only check_http_routes --only check_log_policy
```

结果：全部通过（`check_log_policy` 有 30 条既有 R5 软告警，error 0）。

## 构建

`scripts/build-debug.sh` 成功：

- `output/inotia4_qol_lsposed_debug_2611180133_69f52c861822.apk`
- SHA-256 `69f52c861822df3ed34af177f589739b347494246863c3e3c82d9edb5f5c9293`

## 既有失败（非本批引入）

- `host_adapter_tests` 在 HEAD 即无法链接：`item_port.cpp` 引用 `fn_memorytext_get_text_e` / `fn_cal_calculate`，该目标未编译定义所在源（`game/access/game_access.cpp`）。本批未修改这些文件。
- `test_attribute_quality_adapter.cpp` 的 `kQolPortItemVersion.minor == 6` 与当前 5.7 不符，同为 HEAD 既有漂移；本批未改该文件。
- `world_tuning_feature_tests` 的面板注册计数断言在 HEAD 即失败（`873556ae` 声明式 provider 后未同步）；本批已将断言改为 `panel_available()` 口径，测试转为通过。

## 设备面（NOT_ACCEPTED）

- 需要真机覆盖：105→110 实际升级、升级后属性点/技能点/血量按公式增长、停用后恢复 105、下调到当前等级以下不降级、存档往返后等级不变。
- 以上未取得真机证据前不得宣告 VM-WT-01 通过。
