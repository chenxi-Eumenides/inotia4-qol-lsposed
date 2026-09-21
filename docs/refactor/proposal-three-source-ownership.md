# 提案：依赖来源三档划分（模块提供 / 核心插件提供 / 自行实现）

> 状态：**待审阅提案（尚未实施）**。本文不修改 `ARCHITECTURE.md`，也不表示下列处理已经执行。
> 基线：分支 `refactor/arch-v2`，审阅时点 2026-09-21（审计数据由文末复现命令现场生成）。
> 目的：纠正"插件需要什么就往宿主内核加面"的倾向，明确三类来源的边界、判定测试与升级条件。

## 1. 问题

近期为完成插件边界迁移，宿主内核（`contracts/host_sdk.h`、`contracts/game_host.h` 及其实现）被持续"按需加面"。
这解决了边界问题，但引入了相反的风险：**宿主内核逐渐变成万能层**，把本可由插件自行实现或由提供者插件发布的东西
预置到内核里。审阅要求区分三种来源，并给出可复核的判定标准。

## 2. 三档定义

| 档 | 内容 | 判定测试 | 生命周期 |
|---|---|---|---|
| **① 模块（宿主内核）提供** | 不可委派的宿主事实与机制：游戏版本耦合（地址/符号/ABI/函数调用、变体与目标事实、主线程与帧号）、运行时机制（日志/配置/调度/命令/事件/能力注册/存储容器/面板注册/资源事务/hook 与 patch 安装归还/共享资源分派） | **换一个插件来写，它能不能自己做？** 缺权限、缺事实、或需要唯一仲裁 → 宿主 | 宿主常驻、不可停用 |
| **② 核心插件 / 提供者提供** | 跨多个消费者的共享业务语义，或需要唯一所有权的状态与资源 | **有 ≥2 个真实跨插件消费者，且语义属于某插件领域** → 该插件发布 capability 或 owner command | 普通插件生命周期，可停用/降级 |
| **③ 插件自行实现** | 单消费者逻辑、纯算法、私有状态 | **只有一个使用者，或纯算术/本地状态** → 留在该插件私有头 | 随该插件 |

**升级条件**
- ③ → ②：出现第二、三个消费者，且语义已稳定（避免"提前抽象"）。
- ② → ①：该语义需要唯一仲裁，或必须早于插件系统就绪（自举类，如日志、配置自读、资源登记）。
- **禁止**：为了让某个插件"方便"而把单消费者语义塞进 ① 或 ②。

## 3. 与既有文档的关系

- `ARCHITECTURE.md` §2.2 依赖矩阵：定义"谁能 include 谁"；本提案定义"**功能应该由谁实现**"，二者互补。
- `ARCHITECTURE.md` §7.1 判断顺序（Game Host API / provider / 单消费者）与本提案一致，本提案把它扩展为**可复核的判定测试 + 升级条件 + 审计方法**。
- ADR-0010 的"按需增补"是**迁移期手段**；本提案规定增补的**归属判定**，避免"按需"变成"无原则"。

## 4. 逐条审计（实测，2026-09-21）

审计方法：统计 `contracts/*.h` 在本仓库（排除自身与 `tests/`）的引用文件数，并逐个人工判定引用者是否为"真实跨插件消费者"。

### 4.1 判定为 ①（模块提供）——合规
`host_sdk.h`(19)、`game_host.h`(19)、`plugin.h`(76)、`result.h`(12)、`version.h`(4)、`events.h`(16)、
`commands.h`(12)、`capabilities`相关（经 `host_sdk`）、`panels.h`(13)、`ownership.h`(9)、`ui.h`(36)、`item_count.h`(5)。

### 4.2 判定为 ②（核心插件/提供者）——合规
`attribute_quality.h`(11)、`craft_result.h`(4)、`extension_bag.h`(17)、`inventory_refresh.h`(3)、
`inventory_action.h`(3)、`text_layer.h`(2)、`ui_choice.h`(7)、`http.h`(3，由 `core.api` 提供)。

### 4.3 判定错位——建议处理（**待审阅确认**）

| 现状 | 应为 | 依据（实测） | 建议处理 |
|---|---|---|---|
| `contracts/grid_geometry.h` | ③ 自行实现 | 消费者 **1** 个（`plugins/extension_bag/extension_bag_geometry.cpp`） | 移入该插件私有头；若宿主实现也需要，则落 `game/read/` |
| `contracts/store_sell_rules.h` | ③ 自行实现 | 消费者 **1** 个 | 移入唯一消费者私有 |
| `contracts/game_cache.h` | ① 宿主内部 | 内容是**裸全局函数** `frame_cache_force_refresh()`；使用者全在宿主内部（`game/ops/game_ops_common.cpp` 等 4 处） | 删除该契约，声明收回 `game/cache/` 内部头 |
| `contracts/popup_enter_alias.h` | ① 宿主内部 | 含**内存偏移常量**（`0x10/0x28/0x38`），属不得外泄的布局知识 | 偏移收回 `game/` 层；契约仅保留语义化 POD（如仍有跨层需要） |
| `contracts/in_flight.h` | ① 机制面 | 在途计数是资源机制；当前以契约自由函数形式暴露 | 并入 `host_sdk` 的机制组（保留句柄 POD，去掉自由函数入口） |
| `contracts/autosell.h`(2)、`text_layer.h`(2) | 需复核 | 消费者数为 2，但其中可能含宿主内部消费者 | 逐个列出消费者构成；若只有 1 个真实跨插件消费者 → 降为 ③ |

### 4.4 判定为"保留"的边界情形

| 现状 | 结论 | 理由 |
|---|---|---|
| `contracts/sell_price.h`(4) | **保留**（但复核其中宿主内部消费者） | 纯算术公式、无策略状态；多消费者 |
| `contracts/module_save_port.h`(5)、`contracts/extension_bag_port.h`(8) | **暂留**，登记清除条件 | 过渡期宿主侧端口；待 `api/native` 与根目录遗产吸收后改为能力/宿主内部 |
| `contracts/stack_codec.h` | 已删除 | 已归 `stack_limit` 实现头 |

## 5. 待审阅者回答的问题

1. 三档定义与判定测试是否可操作？是否遗漏了第四类来源？
2. 4.3 的 6 处错位，处理方向是否同意？`grid_geometry` / `store_sell_rules` 这类"纯公式但单消费者"应归 ③ 还是允许留在 ②？
3. `sell_price.h` 保留在 contracts 是否可接受（纯公式、多消费者、无状态）？
4. `popup_enter_alias.h` 的偏移常量：收回 `game/` 后，插件侧的弹窗状态占用是否仍经 `host_sdk.hooks` 表达即可？
5. 是否需要把这套判定测试纳入脚本（如 `check_contracts.py`：单消费者契约告警、裸全局函数硬失败）？若需要，请给出规则清单。

## 6. 复现命令

```bash
# 契约消费者数（排除自身与 tests）
cd module/app/src/main/cpp
for f in contracts/*.h; do
  n=$(grep -rl "$(basename "$f")" . --include='*.cpp' --include='*.h' --include='*.inc' 2>/dev/null \
      | grep -v "^./$f" | grep -v "^./tests/" | wc -l)
  printf '%-32s %s\n' "$(basename "$f")" "$n"
done
```
