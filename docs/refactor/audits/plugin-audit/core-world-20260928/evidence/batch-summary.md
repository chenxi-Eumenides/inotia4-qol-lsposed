# core.world 真机脚本批与人工备料结果

## 交付判定

`NOT_ACCEPTED`：人工视觉卡尚未真人执行；另有 movement 坐标/守卫/teleport 状态码与本次预期不符。材料已保存至本目录，未改源码、正式文档或测试，未提交。

## 部署形态

- 初始运行模块 sha256：`51cddf9a40695eab2f38b3feb20178bb283c407e2007a24a1b56347f74349429`。
- 指定模块 APK sha256：`8edcb179661317e6e79e0a562dbba03600631214c0234df8725bee5ae6c203b6`（文件名前缀 `8edcb1796613`）；不同，因此按指定命令重打 LSPatch 集成包。
- monster v29 集成包：`output/Inotia4_v1.3.2_monster_v29_lspatched.apk`，sha256 `69b034edd2f607a1c5c15d2ac626ed41e80598ffddfe5835ee17c4ea1082ba88`，已 install -r 并重启游戏。
- 运行时模块身份日志为 versionCode 192/versionName 0.7.13、嵌入 APK sha256 `8edcb179...6c203b6`，与输入模块相同。Health `ok=true,ready=true,monster/v29`；agreement 通过 `/api/ui/dialog/select` 关闭，enter_slot 0 ticket 7 done，screen 到 world。
- 启动时仅见短暂 Splash Screen；未发现需要坐标点击的弹窗，无截图点按。

## 备份、读面与移动写面

- Slot 0 在所有写测试前导出并 pull：`backup/20260927-213700_s0_48c05d9ebe79.qol_save`，本地 SHA-256 `917eef3126e8a3e1ee380b78acafe7076273e1492971357f657077a5a4073573`，checksum `48c05d9ebe79`。恢复指令及 SELinux chcon 见 `manual-runbook.md`。
- 7 个 GET 完整 body：`world-map-id.json`、`world-map-exits.json`、`world-map-units.json`、`world-map-enemies.json`、`world-map-interactives.json`、`world-map-drops.json`、`world-map-distance.json`；原始 map=20「黑暗教团礼堂」，player tile `(20,8)`。另外保存当前图静态 info/tiles、map37「帝国首都」info/tiles。
- 对最近历史 `docs/archive/stage5-b4-1-20260922/world-compare-final2.json` 与 drops/enemies 证据：map id/name、exits 字段/键序及 enemies/drops 空数组 schema 一致；units 坐标/单位动态数值有存档/场景态差异，字段顺序与 schema 一致；distance 查询目标不同（本次 `(20,8)`，历史 `(0,0)`），比较 schema，不比较数值。drops 当前仍空；参考 `world-drops.json` 同为空。
- `move_to (21,8)` ACK ticket 8；终态 failed `no path`。Distance/debug 返回 start pixel `(328,136)` / tile `(20,8)`，与契约中 tile 参数口径存在矛盾；不再猜坐标。`walk_dir direction=0` ticket 9 done；`stop_move` ticket 10 done；`interact_with` 无目标 ticket 11 done 并返回 state；同图 teleport 到事实坐标 `(20,8)` ticket 12 done。地图/units 读回及恢复后证据已留存。
- 越界 teleport `(32,5,force:false)` 返回 HTTP 200 + ticket 13，终态 failed `tile (32,5) out of map bounds 32x29 (force to override)`，不是预期同步 HTTP 400。`force:"true"` 同样按 false 被拒，ticket 14 的错误体相同。
- 移动守卫：`x=4294967296` 与 `x=-1` 均 HTTP 400 `x/y required`；`x=134217722` 却被接受并生成 ticket 15，done 后仍在原位；`direction=4` HTTP 400 `direction 0-3 required`。故守卫集未满足预期。

## 级联与 capability

- `core.world` deactivate ticket 16 done；`GET /api/system/features` 显示 5 个 required consumers（autosell/core.quest/world_teleport/extension_bag/inventory_hooks）级联停用。12 条 core.world query/command 路由均用正确方法/body验证为 HTTP 503（错误体 `http owner inactive`；证据 `world-routes-deactivated-status.txt`、`world-deactivated-*.txt`）。
- `core.world` activate ticket 17 done，map/id 回到 200。级联消费者不自动启用，逐一 activate 后状态可恢复；extension_bag 第一次 activate 因依赖次序未齐进入 failed，inventory_hooks 与依赖就绪后重新 activate 恢复 enabled。
- `core.item` deactivate ticket 23 done；`world_teleport`、autosell、inventory_hooks 进入 `installed_disabled/cascade_stopped`，证明依赖 `item.facts` v1.1 的 fail-closed；features 显示 core.item 提供 `item.facts 1.1`，core.world 提供 `world.facts 2.0`。
- 最终逐项恢复核心及相关 consumers；复查 `core.world`、`core.item`、autosell、core.quest、world_teleport、extension_bag、inventory_hooks 均 enabled。`features-restored-final-summary.jsonl` 留存状态。

## Smoke、日志与恢复

- 初次按无参数命令运行时脚本访问 localhost 连接失败；按 docs §3.1/§3.4.4 对应设备链设置 `adb forward tcp:8088 tcp:8088` 后重跑 `uv run python scripts/verification/smoke_all.py`：143 routes，pass=124/warn=19/fail=0。输出 `smoke-output.txt`，JSON `smoke-latest.json`。
- 对比 `scripts/verification/smoke_baseline_v0.7.13.json`：基线 120/23/0，当前 124/19/0；四条 party action 405→400；debug extension_bag/exit_view 200→503（当时 extension_bag 临时处于 failed，后续已恢复 enabled）。细节 `smoke-baseline-diff.json`。Smoke body 上限 400 字符（`routes` 路由登记单独 20000），完整 response body 会截断，这是脚本既有已知限制。
- Smoke 的动作接口不是无副作用：本次观测到 `dialog_wipeout`、凯恩 HP=0；已用先前备份 `checksum=48c05d9ebe79` 导入 slot 0、enter_slot 并等待 world，恢复后 `map_id=20`、player `(20,8)`、HP `8280`、金币 `28550`，screen=`world`、health ready=true。
- 全量日志 `logcat-full.txt`；帧审计 `logcat-frame-audit.json`：459 条解析、无 frame 回退、无未解析 tag 行、`snapshot read failed` warn 为 0。Logcat 可见 core.world/core.item install/disable/deactivate/恢复日志，level/domain 格式匹配；无 snapshot warn 刷屏。

## 人工测试四元组

| 操作 → 验证项 | 关联 VM 卡 | R/B 影响 | 状态 |
|---|---|---|---|
| 7 个 map GET → schema/键序、player 坐标、单位/敌人/交互物/掉落/距离 | VM-K5-04b | 无源码变更，R/B 无变更 | 已执行；保存完整 body；动态字段差异已登记 |
| move_to/walk_dir/stop_move/interact_with/teleport → tickets、移动及输入守卫 | VM-K5-04b；VM-K5-06a~06g、06i | 无源码变更，R/B 无变更 | 部分执行；坐标语义、超界数值守卫和 teleport HTTP 状态未达预期 |
| world/item deactivate/activate → 路由 503、required consumers 级联、item.facts fail-closed、capability 版本 | VM-K2-01/02/03；VM-K5-04b；VM-45 | 无源码变更，R/B 无变更 | 已执行并恢复；5 consumers 状态证据已留存 |
| 人工三态移动、跨图传送、游戏传送 UI 扣费、击杀掉落、暂态/教学拦截 | VM-K5-04b；VM-K5-06a~06g、06i；VM-45；VM-K2-01/02/03 | 无源码变更，R/B 无变更 | 仅备料；需要真人观察/输入，依据 runbook 操作并留截图/录像 |
| smoke + 基线 diff → 143 endpoints、错误差异和 known truncation | VM-K5-04b；VM-K5-06a~06g、06i | 无源码变更，R/B 无变更 | 已执行；pass 124/warn 19/fail 0；包含状态型写调用，导入备份恢复 |

人工操作卡路径：`manual-runbook.md`。
