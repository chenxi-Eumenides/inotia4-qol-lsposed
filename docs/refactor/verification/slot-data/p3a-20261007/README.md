# P3a 验收记录

## 结论与边界

**PASS：P3a Host 机制、oracle 阻断复审及原版 1.3.2/槽 0 受影响正常读面与进档回归。** 不代表完整 VM-K4-04b、VM-K5-06i 重新验收，也不代表 journal v2 已在生产保存事务中接线。VM-SD-01..04 仍为后续计划。

本批修复 SAV-01：共享 store 双损坏不得退化为缺失并由 ensure 写空；同时收口 generation/revision 预约失效、reset/delete IO 失败与 dat evidence 路径绑定。正式契约见 `../../../refactor/slot-data-contract.md`，问题存证见 `../../../refactor/issues.md` 已修复附录，验证后注见 `../../../refactor/verification.md`。

## 已部署构建与验证

- 模块：`inotia4_qol_lsposed_debug_2610071930_466f895b6208.apk`，SHA-256 `466f895b62089ca0c81918bee8ccb529c5a397c496194b3f1affc415d32ebe90`；P2 基线模块 SHA `35bf9d802b99e8a53496ca55cf1777f4ab5531c0dceffedc5a26a1762546f3eb`。
- 原版 LSPatch 集成包 SHA `0b53ccba201e62677be7e5a00f1dde054352ed9b7b1eda284fd5fc2a24e4770a`。实际已安装旧包与新包均经 apksigner 校验签名 SHA-256 `a40da80a59d170caa950cf15c18c454d47a39b26989d8b640ecd745ba71bf5dc`；不从 firstInstallTime 推断签名。
- Host **7/7** 与 Debug 构建通过，oracle 三项 P1 复审通过；构建、Host 的有效输出摘录在 `accepted-evidence/host-tests.txt` 与 `build-result.txt`。收口不重建 APK，避免以未部署构建替换本次有效证据。
- 真机 **88/88** 完整 body 逐字相同：EB status、完整 inventory、items、5 个扩展袋 info、80 个槽；5 袋含 33 件物品。24 个存档目录文件与 11 个模块目录文件的 SHA 不变，原配置逐字恢复。只读 SHA 记录不包含原始 dat/warehouse/module-save 内容。
- 新增/增加 WARN 为 0，崩溃匹配为 0；既有 optional-cycle 与 P_EXT_SAVE_REVIVE 延迟安装告警前后同样存在，不宣称零告警。最终屏幕 main_menu。`accepted-evidence/` 只保留批准的 JSON body、身份/签名输出与机器比较，不提交配置全文、HTTP trace、原始二进制保护备份、APK 或整份 logcat 缓存。

## 安全复现 runbook

1. 使用 `adb -s 192.168.3.54:5555 forward tcp:8088 tcp:8088` 建立访问，完整保存 GET health/screen/features。先以已文档化 `POST /api/system/backup/export {"slot":0}` 导出并拉取保护备份（该接口不调用原版保存），另保留明确存档目录与 module-saves 的只读原字节快照/逐文件 SHA；不要扫描目录取首项或覆盖槽。
2. 本次原 EB 开关为 false，临时启用 `extensionBagEnabled=true` 后重启旧模块、进入已有槽 0；确认 world，采集下面全部 GET 的完整 raw body。未进档或 owner 停用态错误不能作正常档基线。
3. **不要盲跑根目录 patch-apk.sh**（会删除旧 output 集成包）。在项目内任务隔离树复制未修改的脚本，工具/公有签名文件只读引用，仅放入原版 APK，输出也留隔离树。校验集成包嵌入模块 SHA 与两包证书指纹后只 `install -r`；失败不得转卸载/清数据。force-stop+monkey 后轮询 health，若 agreement 才用 API 关闭；核对实际启动模块 SHA，再进入同槽。
4. 以完全相同的 GET 序列取新模块 body，**直接比较字节，不归一化动态字段**：`/api/debug/extension_bag/status`、`/api/item/inventory`、`/api/item/inventory/items`、`/api/item/inventory/bag/{6..10}/info`、`/api/item/inventory/bag/{6..10}/{0..15}`。同槽进档票据终态结果应 entered=true；记录原始日志并单独列出新告警/崩溃，不把旧告警算作消失。
5. 重新核对全部保护文件 SHA，恢复原配置并验证字节，重启至 main_menu。本批不调用原版保存、创建槽、物品写入、导入/删除/回滚、provider 级联或真机损坏注入。原始保护备份与完整采集保留本地原位置，**不删除、不 stage**；提交只使用下述显式安全白名单。

## 提交证据索引

`accepted-evidence/comparison-read-bodies.json` 记录 88 对 body 的独立大小/SHA 比较，正文存于 `raw-bodies.json`：按 SHA 去重的 **42 份完整原始 UTF-8 字符串**，读出字符串后直接 UTF-8 编码即可还原原字节，未重组/归一化 JSON 内容。`save-file-sha256.json` 是去设备目录标识的逐文件大小/SHA 比较；`comparison-warnings.json` 与 `comparison-crash-scan.json` 为日志比较结论；`identity-before.txt`、`identity-after.txt`、签名证书输出与 `verification-summary.json` 锁定构建及范围。`check-arch.txt`、`check-routes.txt` 保存收口静态校验输出。安全提交子集共 **19 个证据文件（含清单）**，`evidence-manifest.json` 校验其余 18 个；不引用未提交的二进制备份或缓存。
