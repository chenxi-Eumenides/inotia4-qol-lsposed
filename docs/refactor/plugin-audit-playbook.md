# 插件审计-修复-测试手册（单插件闭环与多插件并行）

> 供后续插件（core.* owner、功能插件、探针）复用。以 core.world 批（提交 `39b90e4e`…`8cf061ef`）为完整先例。
> 分工：架构规则以 `ARCHITECTURE.md` 为准；证据欠账在 `verification.md` §5；缺陷问题入 `issues.md`；本文件只写**流程**。

## 阶段总览

| 阶段 | 执行者 | 产出 | 出口条件 |
|---|---|---|---|
| 0 准备 | 主代理 | 现场基线 | 设备/模块形态核实 |
| 0.5 插件讲解 | 主代理 | 六维讲解文档（对话输出） | 用户确认理解无误后才进入审计 |
| 0.75 插件参考说明 | 主代理 | `docs/reference/plugins/<plugin-id>.md` 事实说明 | 已按当前源码生成/核对；不是验收结论 |
| 1 静态审计 | @oracle（只读） | findings 表 + 裁决项/可修项分组 | P0/P1 全部有 file:行证据 |
| 2 用户裁决 | 用户 | 逐项决定 | 契约/归属/容量类问题不猜 |
| 3 静态修复批 | @fixer | 代码+host 测试 | host 全绿+构建+一功能一提交 |
| 4 真机脚本批 | @fixer/主代理 | 证据目录 | 读面完整体+守卫+级联+日志+smoke |
| 5 人工目视批 | 用户+主代理取证 | runbook 卡+画面结论 | 每张卡有通过/失败结论 |
| 6 事故分支（可选） | @oracle+@fixer | 根因+最小修复+复验 | 崩溃/死锁不复现 |
| 7 收口 | 主代理 | issues/verification/文档更新 | 才允许写 ACCEPTED |

## 阶段 0：准备

1. 必读：`ARCHITECTURE.md`（§2.2 依赖矩阵、§5 领域/能力、§7 HTTP 契约）、`docs/INDEX.md`、`docs/reference/api-reference.md` 相关节、`docs/refactor/verification.md` 该插件的 VM 卡、`docs/guides/build-and-deploy.md` §3.1。
2. 建立任务隔离目录保存证据、脚本和 runbook；收口后将审计记录迁入 `docs/refactor/audits/`，验证证据迁入 `docs/refactor/verification/`。目录约定见代理规则。
3. 设备与模块形态：`logcat -d -s Inotia4Qol:V | head` 取 `module identity` 行的 sha256，与 `output/` 最新 debug APK 比对。**集成包形态下单独 install 模块 APK 不生效**，需 `scripts/patch-apk.sh <模块apk>` 重打（build-and-deploy 附录）。
4. 确认设备无遗留弹窗/死亡态（`/api/ui/screen` = world 或 main_menu）。

## 阶段 0.5：插件讲解（先于一切审计）

让用户**完整清晰了解该插件的方方面面**后再开始审计。固定六维输出（主代理直接撰写，引用文件:行）：
1. **作用**：架构定位（owner/provider/功能/工具/探针），一句话职责。
2. **功能**：对外提供的全部能力——HTTP 路由表、capability provides、面板/hook 带来的游戏内功能。
3. **使用的模块功能**：requires/optional_requires、ports、Host SDK 面（log/scheduler/commands/ticket_result 等）、消费者。
4. **范围**：职责边界在内/在外（哪些归别的 owner）。
5. **对外提供**：消费者清单与级联影响面。
6. **实现机制**：读路径（快照/节拍/锁）、写路径（HTTP 校验→票据→主线程执行）、生命周期行为。

用户就讲解提问或对某维度要求深挖后，才进入阶段 1。阶段 2 裁决、阶段 3 修复/优化（功能修改、优化诉求也在此受理）依此展开。

## 阶段 0.75：插件参考说明生成/更新（审阅基线）

此阶段与 0.5 的对话讲解、阶段 1 的缺陷审计、阶段 7 的验收登记分开管理。**每个插件审阅都必须先确认一份当前参考说明**；缺失时生成，已有时核对并修正事实漂移。

1. **路径与权威源**：文档放在 `docs/reference/plugins/<plugin-id>.md`；先按 `docs/INDEX.md` 确认目录职责。架构/公共契约以 `ARCHITECTURE.md` 与 `docs/reference/api-reference.md` 为准；当前实现以 descriptor、`routes.inc`、contracts/ports、插件实现和 Kotlin controller/service 源码为准；`archive/` 仅作历史追溯，不作当前事实源。
2. **生成方法**：主代理逐项核对源码后撰写，不从插件名、旧 audit 或测试输出推断行为。至少覆盖：职责/owner、路由与公开能力、requires/ports/消费者、职责边界、关键读写/线程/生命周期机制、文件地图；关键事实标 `path:line`。无法确认的事实标为未知，不猜测。
3. **已有文档处理**：若文档已存在，逐节对照当前实现，只修正过时或缺失的事实；公共 HTTP 契约仍只维护在 API 参考，不在插件说明中复制完整 schema。新建插件说明时同步更新 `docs/INDEX.md` 插件参考链接。
4. **与审计/验收的边界**：插件说明记录稳定实现事实，不登记 findings、用户裁决来源、运行状态结论或“已验收”标记；问题记入 `issues.md`，设备/Host 验收与跳过项记入审计 `verification-log.md` 和 `verification.md`。文档生成不能替代阶段 1 的独立审计，也不能把未完成 VM 卡写成通过。
5. **出口检查**：确认文中路径/符号/能力/owner 与源码及 API 契约一致；`git diff --check` 通过。阶段 3 仍遵守 fixer 只改代码与测试；代码修复后由主代理回到本阶段的核对清单更新参考说明，再进入阶段 4/5。

## 阶段 1：静态审计（@oracle，只读）

审计维度模板（A–G，逐维给结论）：
- **A 契约符合性**：descriptor（requires/provides/resources/jni/http_queries）vs 实现；routes.inc owner 归属；冻结面（路径/方法/成功响应/NativeBridge external）。
- **B 依赖方向**：§2.2 矩阵；`check_arch.py` 基线与豁免是否掩盖真实耦合；运行时 `capabilities.require` 是否声明（静态检查盲区，ITM-02 教训）。
- **C 代码质量**：裸 VMA/偏移（必须 `game_symbols.h`）；错误语义与类型；输入解析边界（数字溢出、字符串类型）；缓存/快照失效竞态。
- **D 并发纪律**：锁内调用可重入路径（EB `g_virtual_bag_mtx`×`op_ok()` 模式为范例）；跨线程游戏内存读写。
- **E 测试缺口**：host target 覆盖面 vs 错误路径/边界/并发；golden fixture 是否漏袋/漏态（bag5 教训）。
- **F 文档漂移**：api-reference 示例 vs 实现真值（登记入 issues.md §9，本批不改文档）。
- **G 基础设施顺带检查**：该插件途经的 registry/dispatch/command_queue/cache/patch 机制问题——**单列成基础设施观察，不混入插件缺陷**。

产出要求：Findings 表（编号|严重度|文件:行|证据|影响|建议|是否需真机）+ 两组清单：**需用户裁决**（契约语义、领域归属、容量策略、去留）与**可直接修**（守卫、断言、日志、测试、明显 bug）。

## 阶段 2：用户裁决

裁决问题一次问全（每条给候选+推荐）。典型类别与本项目先例：
- 领域归属（money→item.facts；tutorial_state 留 world）。
- 容量/超限语义（票据 done+省略结果 vs failed）。
- 冻结响应是否可加字段（stale 标记）。
- 行为统一的选择（越界目标走最近可达 vs 拒绝）。

## 阶段 3：静态修复批（@fixer）

约束模板（每次下发必写）：
- 只改代码与测试；**文档不动**（验收后统一改）；不改版本号；不 commit（主代理验证后提交）；不碰设备。
- 冻结面逐字不变；新错误路径复用既有文案；注释只写事实与原因。
- 交付报告强制四元组：触及文件 → 影响编号（issues.md ID/审计编号/R-xx）→ 关联 VM 卡 → 已执行/无法执行原因。

验证门（全绿才提交）：host ctest（新用例+受影响 target）、`check_arch.py`、`check_http_routes.py`（143 不漂移）、`scripts/build-debug.sh`、`git diff --check`。**一功能一提交**。

## 阶段 4：真机脚本批（无需真人）

顺序与要点：
1. **部署**：比对模块 sha → 必要时重打集成包 → `install -r` → force-stop+monkey →（启动弹窗需坐标=转人工）→ health 轮询 → API 关同意页 → enter_slot。
2. **备份先行**：写测试前导出存档 + `adb pull` + sha256（恢复路径同批验证——本先例中它救回了一次团灭与一次驻留锁死）。
3. **读面**：逐条 GET 保存**完整 body**（smoke 的 400 字符截断不可作逐字依据，TL-01）；与历史基线比 schema/键序，动态差异登记。
4. **写面**：正常流（ACK→`/api/system/tickets/{id}` 终态→result 结构）；守卫流（超界数字/负值/字符串布尔/溢出乘法 → 既有 400 文案且**不产生票据**）。
5. **生命周期**：deactivate → owner 路由 503 + `/api/system/features` 消费者级联状态；activate → 恢复 + 复查（注意消费者不自动恢复，需逐个 activate；失败先查依赖序）。
6. **日志**：全程 logcat；帧号单调、level/domain 正确、限速告警不刷屏。
7. **smoke**：全量跑 + diff 基线（写副作用风险 TL-02：动作面可致团灭，备份必须先做）。

**分诊纪律（重要先例教训）**：结果与预期不符时，先分辨「审计/主代理预期写错」还是「真缺陷」。本先例三处“差异”两处为预期错（INT32 乘法界算错、越界校验误期望同步 400——异步票据契约下执行期校验属正确设计），一处为环境问题。判完再立案，避免误修。

## 阶段 5：人工目视批

1. 主代理预生成 `manual-runbook.md`：备份恢复指令（含 SELinux chcon 注意项）+ 每卡（目的→前置→可复制命令→预期现象→取证要求→预计时长→失败回滚）。
2. 典型卡：移动三态目视（可达/被隔/越界）、转场动画、**游戏内 UI 扣费**（能力迁移类改动的唯一端到端证据）、杀怪产掉落+拾取、UI 占据暂态、级联停用游戏内表现、（可选）帧率感知。
3. 用户操作时主代理同步轮询 API/抓 logcat 留证；卡结果逐条记「通过/失败+现象」。
4. 环境致命性预案：高伤区域用短窗口协议（进图→动作→限时撤出）；死亡链上不做任何未验证的 API 驱动。

## 阶段 6：事故分支（卡死/崩溃/静默无效）

按 build-and-deploy §3.3 + 本先例：
1. **热取证**：`debuggerd -b <pid>` 全线程栈、logcat、API 活性（health/screen）、窗口焦点。
2. **栈符号化**：模块 `.so` 用 `tools/ndk/.../llvm-addr2line`/`llvm-nm`（对 `build/intermediates/cxx/Debug/*/obj/arm64-v8a/` 带符号版）；游戏 `libgame.so` 从 `apk/game-apk/<变体>.apk` 提取 + `llvm-objdump -d` 反汇编崩溃 PC 前后。
3. **@oracle 只读定位**：给栈+代码路径，要求完整等待链/调用链，区分本次改动 vs 既有机制。
4. **归属实验**：受控复现到最小触发步（先静置、再逐步动作；每步取证 tombstone 增量）；「模块 vs 游戏自身」的分界靠**同一动作的人工对照**（先例：手动点 OK 正常 → 判定模块 handler 选错而非游戏必崩）。
5. **API 静默无效类**（CHR-01 范式）：读路径证活（战斗中值变化）→ 高频采样抓「写入瞬时值」判定镜像覆盖 → 对照实验（同链路另一 owner 写生效）→ frida 直写定位权威字段。
6. 修复批按阶段 3 流程走，**host 用例必须复现原事故**（同步重入、回写常量、fail-closed），再真机「引入→修复→三步复验」闭环。
7. 结论入 issues.md：修复条目直接进附录；新开放问题入正文对应组件节。

## 阶段 7：收口清单（ACCEPTED 前逐项核对）

- [ ] 全部 commit 在链（host 批 / 迁移批 / 事故修复批）。
- [ ] **验收登记文档**：`docs/refactor/audits/<plugin>-<YYYYMMDD>/verification-log.md`，章节顺序固定＝ §1 本批登记问题（指针到 issues.md）→ **§2 无法验证项** → §3 已完成验收登记（表：验收项|结果|证据）。证据副本入同目录 `evidence/`；当前验收证据不得写入 `docs/archive/`。
- [ ] **无法验证项**：环境/存档缺失等无法验证的项，经**用户确认后**登记于验收登记文档 §2（倒数第二部分，已完成之前、问题之后），并同步 `issues.md` §10「无法验证项」；此后**不再主动验收**。用户确认"已完成"的项按用户口径登记，但如实注明确认覆盖范围（哪些是专项实测、哪些是总体确认）。
- [ ] `verification.md`：受影响 VM 卡行末追加一行「插件审计批 <日期>」后注，指向验收登记文档；新欠账登记 §5。
- [ ] `issues.md`：新发现缺陷全部入账（含归属），已修条目在文末附录；§10 无法验证项同步。
- [ ] `docs/reference/plugins/<plugin-id>.md` 已按阶段 0.75 与最终实现同步；新文档已加入 `docs/INDEX.md`。该文档只写事实，不暗示未完成的 VM 验收已通过。
- [ ] API 契约文档仅在公共 API 契约实际变化时更新；验收状态、缺陷与跳过原因分别登记在 audit log、`verification.md`、`issues.md`，不混入插件事实说明。
- [ ] 设备恢复默认功能集（隔离测试期间的 deactivate/config 改动逐项还原并验证）。

## 多插件并行

- **可并行**：阶段 1 静态审计（@oracle 只读，互不干扰）；阶段 3 中互不重叠文件域的修复批；core.* owner 与其消费者插件的审计并行（消费者只查接口交界，不改 provider）。
- **必须串行**：阶段 4/5（单设备、单存档态）；commit 与构建设备部署；任何触及同一契约面（routes.inc、contracts 头）的修改——一次一批，防基线互踩。
- **推荐流水**：插件 A 的真机批进行时，并行跑插件 B 的静态审计 + C 的修复批（写域不相交）。
- **跨插件同型问题集中裁决**：如附容量语义（ITM-03/04/05、CHR 侧）应一批处理；引擎级问题（ENG-01 级联 deactivate）立项为独立任务，不夹带在单插件批里。
- **消费者驱动原则**：owner 插件的能力面变更（版本升降、成员增删）必须连同全部消费者与其 host stub 一起改完再验证装配（先例：world.facts 2.0 与 5 消费者 + capability DAG 协商真机复验）。

## 常用命令/工具速查

- 部署链：`scripts/build-debug.sh` → `scripts/patch-apk.sh <apk>` → `adb -s <dev> install -r` → force-stop+monkey → curl `/api/health`。
- 取证：`adb logcat -s Inotia4Qol:V`、`su -c 'debuggerd -b <pid>'`、`ls -t /data/tombstones/`、`curl /api/system/tickets/{id}`。
- 符号化：`llvm-nm -C`、`llvm-addr2line -e <so> -f -C <addr>`、`llvm-objdump -d --start-address --stop-address`。
- 运行时探针：`tools/frida/frida-server` 推 `/data/local/tmp` + `su -c nohup` 启动；Python 侧 `.venv` frida attach 进程显示名。
- 校验：`uv run python scripts/maintenance/{check_arch,check_http_routes,check_resources}.py`、`scripts/verification/smoke_all.py`（注意写副作用与截断限制）。
