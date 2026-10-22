# `craft_lottery` 验证登记（2026-10-22）

## 1. 静态与 Host 证据

| 项 | 结果 | 证据 |
|---|---|---|
| 全量 Host CTest | **153/153 PASS** | `.tmp/craft-lottery-20261022/host-ctest.log` |
| check_arch | offsets / jni / resources 全绿；check_layers 仅既有 `custom_map` 6 条越层（commit 5140b0af，与本批无关） | `.tmp/craft-lottery-20261022/check-arch.log` |
| git diff --check | clean | 同上目录 |
| Debug 构建 | BUILD SUCCESSFUL，APK `output/inotia4_qol_lsposed_debug_2610230158_2c46a664c644.apk`，SHA-256 `2c46a664c64442faaf9ce9e486a1d317d7a26a599c6bf3da1200f9ddd0515d7d`（2026-10-23 追加行格式修正后复验：CTest 153/153、check_arch 除既有 custom_map 外全绿） | `.tmp/craft-lottery-20261022/{build-debug.log,check-arch-r2.log}` |

## 2. 逆向依据（静态）

- `CHAR_ProcessRecipe` / `ITEMSYSTEM_DecideRecipeID` / `MIXSYSTEM_AddRecipeBook` / 配方书位图 / ITEMMIXLINKBASE 池结构与套装映射：见 design-card §2（objdump 输出留存 `.tmp/craft-lottery-recon/`）。

## 3. VM-CL-01

**NOT_ACCEPTED（待真机）**。步骤与预期见 design-card §6。当前无任何设备部署与行为证据；Host 测试与构建成功不构成行为验收。
