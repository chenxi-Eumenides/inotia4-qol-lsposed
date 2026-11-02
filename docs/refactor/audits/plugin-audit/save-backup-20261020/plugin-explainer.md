# `save_backup` 插件讲解（阶段 0.5）

> 依据当前源码与 `docs/reference/plugins/save_backup.md`（阶段 0.75 参考说明，2026-10-20 新建）。文件路径省略公共前缀 `module/app/src/main/cpp/plugins/save_backup/`。

## 1. 作用

`save_backup` 是**存档备份管理功能插件**（ID `save_backup` 0.1.0），不是领域 owner：它不提供 capability，不拥有存档事实或写路由。职责是 `.qol_save` 备份文件的导出/列表/导入/删除、游戏内备份面板，以及“内存档面板删档后同步清理同槽 sidecar”的 GOT 挂钩（`plugin.cpp:25-26,54-120,341-369`）。

存档事实、进档/建档、保存协调属于 `core.save`（`docs/reference/plugins/core.save.md:58-60`）。

## 2. 功能

| 面 | 内容 |
|---|---|
| HTTP | `POST /api/system/backup/export`、`GET /api/system/backup/list`、`POST /api/system/backup/import`、`POST /api/system/backup/delete`；descriptor 声明、宿主托管注册/墓碑；**同步 handler、无票据**（`plugin.cpp:176-239`） |
| 能力 | required：`ui_panels` 1.3、`save.facts` 1.0；optional：`http.registry`/`http.dispatch`；provides 无（`plugin.cpp:31-47,350-352`） |
| 资源 | `ptrslot:SAVESLOT_DELETE_GOT`（required/fail-closed）、`own:popup.savebackup`（required/fail-soft）（`plugin.cpp:88-120`） |
| 游戏内 UI | 设置 → Tools 的 entry「存档备份导出/打开」，push 到备份面板；面板三栏：3 槽位、导出/导入/删除、备份列表（5 项/页，条目数不设上限）（`plugin.cpp:341-345`；`ui/panel.cpp:74-116,413,515-522`） |
| 面板操作 | 直接调用 native 业务函数，不经 HTTP/JNI；导入与两类删除均两步确认（`ui/injection.inc:22-188`） |

## 3. 使用的模块功能

- `save.facts`：`load_data` 取原版明文，`slot_exists/slot_hero_index/hero_level/hero_class/hub_key` 取元数据与密钥（`feature.cpp:491-557`）。
- `QOL_PORT_SAVE_CALL`：`write.free_data` 释放明文、`write.encrypt` 加解密存档与 wh4、`write.mark_stale` 删档后失效槽缓存（`feature.cpp:194-223,560-589,738-757`）。
- Host SDK：capability require、panel provider 注册、owner-bound store 读写 sidecar（`.qol-save`/`.last-good`）、日志（`feature.cpp:48-51,194-223`；`plugin.cpp:241-291`）。
- 宿主机制：HTTP endpoint 注册与 503 墓碑、slot operation lease、GOT ptrslot 安装与还原、PopupState ownership（不在插件内实现）。

## 4. 范围（边界内外）

**在内**：bundle 格式（QSB1 v1/v2/v3）、`externalFilesDir/backups/`、checksum 去重、四个文件级操作、备份面板、`open_manager` entry、删档 sidecar 清理。

**在外**：`save`/`enter_slot`/`create_slot` 与槽事实（`core.save`）；回主菜单（`core.ui_state`）；面板栈/控件/文本（`core.custom_ui`/`ui_panels`）；HTTP 路由注册与调度（宿主/`core.api`）；`slot_data` 参与者（`core.save`）。

## 5. 对外提供与级联面

- 四条 HTTP 路由：Kotlin `SaveController` 校验后透传（`module/app/src/main/java/com/inotia4/qol/api/controller/SaveController.kt:39-79`）。
- 设置 entry 与面板：settings/catalog 渲染，`ui_panels.open("save_backup")` 打开；面板嵌套在设置页之上，返回回设置页（`ui/panel.cpp:701-750`）。
- 无 capability 消费者，因此停用不触发能力级降级；停用影响面 = 4 条路由 503 + 设置 entry/面板不可用 + 删档 hook 资源释放（`plugin.cpp:274-291`）。
- 存档数据影响面：导入会**覆盖目标槽 dat、sidecar 与 wh4 仓库文件**，失败按序回滚并保留证据；备份文件本身由 `save_backup` 独占。

## 6. 实现机制

- **读路径**：`list` 在 `g_sb_mtx` 内枚举 `backups/`、逐文件全量读入并解析 CRC、跳过损坏项、按 `export_time` 降序（`feature.cpp:782-824`）；面板刷新另起 detached 线程，锁外 IO（`ui/panel.cpp:561-603`）。
- **写路径**：export 锁内组装 bundle → 去重 → `.tmp` + `rename`（`feature.cpp:827-1010`）；import 先解析、释放插件锁、acquire 槽 lease，再备份 dat/sidecar → 原子写 → 逆序回滚（`feature.cpp:1033-1282`）。
- **线程**：HTTP handler 在 JNI dispatch 线程同步执行业务；槽元数据读取与槽刷新在游戏线程；面板事件在 PopupState event 上下文（`ui/panel.inc:181-343`）。
- **生命周期**：install 绑定能力/port 并注册 panel provider；enabled/disabled 仅翻 `g_active`（不门控业务）；deactivate 撤销 owner 面板、注销 provider、解绑上下文（`plugin.cpp:241-312`）。
- **格式**：QSB1、v3 大端分段，尾部 CRC32；checksum = `sha256(plain‖module‖warehouseBlob)` 前 12 位；文件名 `时间戳_s槽_checksum.qol_save`，碰撞追加 `_1.._99`（`bundle.h:10-29`；`feature.cpp:953-964,998-1009`）。

## 待用户确认

1. 上面对“作用/边界/级联面”的定位是否与你的理解一致。
2. 是否有你关心的特定维度需要先深挖（例如导入回滚、面板交互、跨设备 wh4 重加密）。
3. 本批是否包含真机阶段（4/5）与修复（3），还是先只出静态审计与裁决清单。

确认后进入阶段 2 裁决与阶段 3 修复批。
