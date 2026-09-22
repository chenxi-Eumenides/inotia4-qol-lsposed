# VM-K5-02：阶段 5 批 2——静态/低风险查询路由 owner 真实化

## 变更范围

- `/api/config/list`、`/api/config/set`：`api.config` → `module.config`。
- 四条静态地图路由：`api.world` → `static.data`。
- 八条静态表/帮助/下载路由：`api.system` → `static.data`。
- `/api/quest/{id}` 保持 `api.quest`：`InfoApiServiceCore.questListId` 直接返回 `notFound()`，没有静态数据读取，不能归入 `static.data`。
- `op_gated`、`lease_required`、Kotlin Controller/Service 和 NativeBridge 均未修改。

## 验证结果

- `routes.inc`：143 条；设备 `/api/system/routes`：`registered=143`、`expected=143`、`duplicates=0`、`omitted=0`、`accepting=true`。
- 设备 owner：`module.config=2`、`static.data=12`；14 条 owner 变化仅出现在 `/api/system/routes` 响应中。
- Smoke：`total=143、pass=120、warn=23、fail=0`；warn 数与批 1 基线相同。
- Host CTest：58/58 通过；架构检查全部通过。
- Debug APK：`inotia4_qol_lsposed_debug_2609222023_05a6093b44b1.apk`，构建产物 `plugin_descriptor` 字符串计数 23，`kernel_only=false`。
- Monster v29 LSPatch 集成包安装后：health ready，进档后 `screen=world`；装配日志 `registered=23 install_failed=0 kernel_only=false`；清空旧 logcat 后本次启动日志无 FATAL、SIGSEGV 或 UnsatisfiedLinkError。

## 基线口径

`smoke_all.py` 对 `/api/system/routes` 保留完整响应概要，使 14 个 owner 变化进入冻结基线。与批 1 基线比较时，142 条非诊断路由的 status、ok、verdict、reason、error 均未变化；7 条查询响应的 body 含运行时指针、帧号或当前存档状态，字节级 body 差异属于动态数据，不作为 owner 迁移回归。

完整基线：`scripts/verification/smoke_baseline_v0.7.13.json`。
