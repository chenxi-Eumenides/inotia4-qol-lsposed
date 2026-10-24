# `extension_bag` 迁移基线（v2 重构 Step 0）

测量日期：2026-10-25。用于每一步重构前后对比；数字只统计 `module/app/src/main/cpp/plugins/extension_bag`。

## 1. L4 对 live State 的直接访问

| 层 | 文件数 | `runtime_state()` 引用 |
|---|---:|---:|
| `api/` | 1 | 49 |
| `http/` | 0 | 0 |
| `public/` | 5 | 358 |
| `ui/` | 7 | 528 |
| **合计** | **13** | **935** |

L4 内锁获取点（`lock_guard` / `unique_lock`）：**108**。

复测命令：

```bash
cd module/app/src/main/cpp/plugins/extension_bag
for d in api http public ui; do
  printf '%s files=%s refs=%s\n' "$d" \
    "$(grep -rl 'runtime_state()' $d | wc -l)" \
    "$(grep -r 'runtime_state()' $d | wc -l)"
done
grep -rE 'lock_guard|unique_lock' api http public ui | wc -l
```

## 2. 互斥量

| 名称 | 获取点 | 保护对象 | 目标 |
|---|---:|---|---|
| `runtime_state().mtx` | 116 | objects / ui / store / persistence / ownership | 删除（单线程 + 快照） |
| `g_c18_layout_diagnostic_mutex` | 1 | 诊断去重表 | 保留或改主线程表 |

`try_to_lock` 7 处：`public/bridge.cpp:265`、`runtime/ownership.cpp:207`、
`ui/detail_actions.cpp:367,478`、`runtime/tabs_ui.cpp:89,110,183`。
它们不是递归检测，需替换为显式重入标志/入口守卫。

## 3. 入口清单

| 入口 | 线程 | 现状 | 目标 |
|---|---|---|---|
| HTTP POST（EB 命令） | JNI/HTTP 线程提交 | `post_typed` → 主线程 | 保持 |
| HTTP GET（status/capacity/inventory） | JNI/HTTP 线程同步 | 读快照（无 fallback） | 保持并统一走 BagQuery |
| JNI/public 查询 | 任意 | 部分 live State | 读快照 |
| JNI/public 写 | 任意 | 部分直接执行 | `post_typed` + ticket |
| `ExtensionBagApi` provider | 调用方线程 | 直接读写 State | 读快照；写转 command |
| 中立 inventory provider（返回指针） | 主线程（游戏 hook） | live 指针 | 契约限定主线程 |
| UI 回调 / draw hook | 主线程 | 直接访问 State | 用例 + 快照读 |
| slot_data load/capture | 主线程存档点 | live State | 保持主线程 |

## 4. 不能快照的读

| 接口 | 原因 | 约束 |
|---|---|---|
| `item_ref_at` | 返回 native 物品指针 | 主线程；调用方立即消费 |
| `for_each_item` | 回调暴露指针/句柄 | 主线程；回调内不得再入 |
| `physical_item_at` | 返回物理槽指针 | 主线程 |
| 投影 install/restore | 修改原生控件/current_bag | 主线程；ProjectionPort |

## 5. 每步验收门槛

| 步骤 | 门槛 |
|---|---|
| Step 1 BagQuery | L4 只读引用数下降；CTest 全绿；无 JSON 变化 |
| Step 2 BagCommand 试点 | `set_bag_count` 走票据；HTTP 行为不变；无 off-main 日志 |
| Step 3 Command 全量 | 四方向移动/装备/出售回归；L4 写路径无 live State |
| Step 4 数据层 | 用例不 include `runtime/state.h`；数据组件单测 |
| Step 5 删锁 | 200 次混合命令压力；无重入拒绝死循环 |
| Step 6 CI | `check_arch` 新规则通过 |

## 6. 当前状态

- Step 0：本文件。
- Step 1：完成（`8a7ff628`，`interface/bag_query.*`）。
- Step 2：进行中（`interface/bag_command.*` + `set_bag_count` 用例化）。
- `runtime_state().mtx` 仍为 116 个获取点；`try_lock` 7 处。
