# 推怪（push-all）

> 状态：已实现（config.json / `/api/config/set` / 游戏内设置页第 2 页「推怪」三处接线完成；host tests、debug 构建与开关链路的真机验证通过；**实际推开怪物/队友/召唤物的游戏内行为未取真机证据**）｜位置：`module/app/src/main/cpp/feature/push_all/`
> 开关：模块设置页第 2 页「推怪」／`POST /api/config/set {"pushAllEnabled":true}`／`config.json` 的 `pushAllEnabled`；默认 `false`

## 1. 语义

开启后，主控与队友移动时可推开挡路的**怪物 / 队友 / 召唤物**；**场景设施（NPC、商人等）保持不可推**。关闭后恢复原版行为。

原版规则：`CHAR_Move` 推动分支要求 blocker 同时满足「在队伍中」与「处于站立状态」两个条件。因此原版里怪物（不在队伍）与战斗/行动中的单位（非站立）都推不动。

## 2. 闸门落点

| 闸门 | 位置 | 原指令 | patch 后 |
|---|---|---|---|
| 队伍闸门 | `CHAR_Move+0x168` | `bl CHAR_GetPartyIndex(blocker)` | BL 重定向到模块 wrapper `push_all_party_filter` |
| 站立闸门 | `CHAR_Move+0x180` | `cbz w0, <推动分支外>` | `NOP`（`ARM64_NOP_WORD`） |

常量集中在 `game/symbols/game_symbols.h`（`F_CHAR_MOVE_VMA`、`CHAR_MOVE_PUSH_PARTY_BL_OFF/WORD`、`CHAR_MOVE_PUSH_STAND_CBZ_OFF/WORD`、`ARM64_NOP_WORD`），域文件不写裸偏移。

**wrapper 资格矩阵**（`push_all_party_filter`，返回 `0` = 可推，`-1` = 不可推）：

| 输入 | 判定 | 依据 |
|---|---|---|
| `nullptr` | -1 | 防御 |
| `C_TYPE == 1`（怪物） | 0 | 本功能新增语义 |
| `C_TYPE == 0/2` 且 `CHAR_IsActivePlayerGroup(ch) != 0` | 0 | 主控及其召唤物群体 |
| 其余 | 原版 `CHAR_GetPartyIndex(ch)` | 队友 `>=0` 可推；场景设施 `-1` 不可推 |

`C_TYPE` 取值语义：0=主控、1=怪物、2=队友/NPC/装饰物/召唤物混合。**`C_TYPE == 2` 不能直接判不可推**——实测队友（祭司、拉卡诺斯）也是 `type=2`，必须靠 `CHAR_GetPartyIndex` 区分队友与场景设施。

wrapper 在游戏主线程 `CHAR_Move` 调用链内执行，只读 `C_TYPE` 并调用纯内存读的 `fn_*`，不引入新的线程安全面。

## 3. 开关接线

`config.json`、`POST /api/config/set` 和设置页开关是同一份配置的三个入口：

`ModuleConfig.pushAllEnabled` → `ConfigApiService.applyToNative()`（启动期）或 `applyOnChange()`（变更期）→ `NativeBridge.nativeSetPushAllEnabled` → JNI `gamebridge_push_all.cpp` → `feature/push_all/push_all.cpp` → `patch/call_patch`。

设置页路径：`ModuleConfigUiBridge.toggleConfig("pushAllEnabled")`（与其它开关同构，只允许翻转布尔项）。开关为持久化配置，启动期由 `applyToNative()` 重新下发，状态跨进程重启保持。

## 4. 失效与还原语义

- **fail-closed**：安装前校验目标指令字；不符即拒绝且不改写任何字节。返回值只表示「本次下发是否成功」，上层 `ConfigApiService` 记录日志，不回滚已持久化的配置值。
- **部分失败回滚**：开启时先装队伍闸门，再装站立闸门；站立闸门失败会回滚队伍闸门。否则只剩队伍闸门生效（怪物仅在站立时可推），与开关状态不符且调用方无法感知。
- **幂等**：`call_patch_*` 的 install/revert 对已安装/已还原状态重复调用返回 `true`，因此开→开、关→关重复下发仍成功。
- **关闭失败**：两条闸门都尝试还原，任一失败即报告失败。
- **空指针防御**：`fn_char_is_active_player_group` / `fn_char_get_party_index` 任一未解析时判不可推（fail-safe），避免在 `CHAR_Move` 调用链内解引用空函数指针。

### 设计决策（思考）

- **为什么 patch 两个闸门而不是包裹整个 `CHAR_Move`**：`CHAR_Move` 是移动主路径，包裹它会接管全部移动语义（寻路、切图、按键链），风险面远大于只放开推动分支的两个资格判定。两个闸门都是单条指令改写，可用 `call_patch` 的幂等原语实现，且失败可精确还原。
- **为什么开关是持久化配置而不是运行时 OP 端点**：该能力是玩法增强，使用形态是「开启后长期生效」，需要跨重启保持、在游戏内可见可切换；OP 域是越权数据改写的隔离区，配置项与功能开关都不应放在那里。原 `POST /api/op/movement/push_all` 端点已删除。
- **为什么开启时第二条闸门失败要回滚第一条**：两闸门必须同相才有意义；只装队伍闸门会让「怪物可推」这一新增语义在非站立时不生效，且外部无法从返回值区分「完全未安装」与「半安装」。
- **为什么日志归 `platform` 域**：本功能只产生 patch 安装/还原诊断，`docs/development/logging.md` §3 把 `call_patch` 归入 `platform`。

## 5. 版本兼容

原版 / 大修 / monster 三版符号名一致但 VMA 不同 → 实现经 `g_base + 符号 VMA` 常量定位，不做版本分支。

32 位构建下偏移处的指令字与常量不匹配，安装被 fail-closed 自然拒绝（模块仅支持 arm64-v8a）。

## 6. 验收计划与证据状态

`git diff --check` 通过；host tests `ctest` 16/16 通过；`scripts/build-debug.sh` BUILD SUCCESSFUL（`output/inotia4_qol_lsposed_debug_2609162124_c7aefc747c7e.apk`）。

| # | 验收项 | 证据 | 状态 |
|---|---|---|---|
| VM-P1 | 设置页第 2 页「推怪」开关翻转后，设置页状态与 config.json 一致 | 设置页第 2 页第 3 行「推怪」（翻转前后分别显示 关 / 开）+ 日志 `toggle pushAllEnabled -> ok` + `config.json` | **通过** |
| VM-P2 | `/api/config/set {"pushAllEnabled":true/false}` 下发后 native 安装/还原成功 | native `platform` 域 `call_patch` 日志 | **通过** |
| VM-P3 | 开启后可推开怪物 / 队友 / 召唤物 | 真机操作 | 待执行 |
| VM-P4 | 开启后场景设施（NPC/商人）仍不可推 | 真机操作 | 待执行 |
| VM-P5 | 关闭后恢复原版行为（怪物不可推） | 真机操作 | 待执行 |
| VM-P6 | 进程重启后配置保持且 patch 重新安装 | 重启后 native 日志 | **通过** |

VM-P1/P2/P6 真机证据（原版游戏包，debug 构建 `inotia4_qol_lsposed_debug_2609162124_c7aefc747c7e.apk`）：

```text
# 启动期（config.json 缺 pushAllEnabled → 自动补齐并落盘；applyToNative 下发）
I config ConfigApiService.kt:54 pushAllEnabled=false applied=true
I config ModuleConfig.kt:306 config persisted: .../files/config.json

# 开启（POST /api/config/set {"pushAllEnabled":true}）
I config ModuleConfig.kt:306 config persisted: ...
I platform call_patch.cpp:188 patched call=0x7a50706970 replacement=0x94194dd4
I platform call_patch.cpp:228 patched word addr=0x7a50706988 new=0xd503201f

# 关闭（POST /api/config/set {"pushAllEnabled":false}）
I platform call_patch.cpp:240 reverted word addr=0x7a50706988 original=0x34fffde0
I platform call_patch.cpp:210 reverted call=0x7a50706970 original=0x97ffcc28

# 设置页开关（点击第 2 页「推怪」→ 关→开）
I ui game_ui_settings_config.inc:55 toggle pushAllEnabled -> ok
I platform call_patch.cpp:188 patched call=0x7a50706970 replacement=0x94194ddc
I platform call_patch.cpp:228 patched word addr=0x7a50706988 new=0xd503201f
```

`0x7a50706970`/`0x7a50706988` 与 `g_base + F_CHAR_MOVE_VMA + 0x168 / 0x180` 一致；还原日志里的原指令字 `0x34fffde0`/`0x97ffcc28` 与 `game_symbols.h` 常量逐位相同，说明 fail-closed 校验锚点与真机二进制一致。

未执行项：`VM-P3`–`VM-P5` 需要游戏内实际移动与观察单位位移，未取证据。按项目交付定义，行为面缺少真机证据时整体只能报告 **`NOT_ACCEPTED`**。
