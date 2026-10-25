# custom_recipe 架构整改方案

状态：阶段 1 已实施（Host 4/4 + Android Debug 构建通过；真机 smoke 待执行）；阶段 2/3 未实施
范围：`module/app/src/main/cpp/plugins/custom_recipe`（4,847 行 / 18 文件）
依据：`docs/refactor/plugin-architecture-guide.md`（下称"指南"）、`ARCHITECTURE.md`、`docs/refactor/audits/plugin-audit/custom-recipe-20261014/static-audit.md`

## 0. 结论摘要

指南的绝大部分条款本插件已经满足，**不做** extension_bag 式的整体重构。本方案只处理三件事：

| 阶段 | 内容 | 性质 | 预估 |
|---|---|---|---|
| 1 | 表槽位地址下沉到 game 适配层（新窄端口） | **必修**（违反 `ARCHITECTURE.md:74,351`，且检查器漏检） | 2 小时 |
| 2 | 目录分层迁移（纯移动，无行为变化） | 可选，建议做 | 45 分钟 |
| 3 | 主线程/重入契约显式化（日志级，不改语义） | 可选 | 30 分钟 |

不做的部分见 §7，包括查询快照、外部写队列、JSON 接口层、L2 持久化、配方 `.inc` 生成。

## 1. 现状基线

### 1.1 已满足的指南条款

| 指南条款 | 现状证据 |
|---|---|
| §1 依赖只向下 | `feature.cpp:10-24` 只 include `feature.h`、`contracts/`、`catalog.h`、`rules.h`、`table.h`；无 `ui/*`、无 UIMix 类型。`rules.cpp:1-4` 仅标准库。 |
| §7 聚合头 / 宏桥 | 均为 0（grep `_internal`、`#define <name> <name>`）。 |
| §5 / §7 JSON 进用例 | 业务层无 `nlohmann/json/parse(` 命中；`result.h:55-108` 是 typed POD + 函数指针表。 |
| §2.1 控件不进核心 | 非 UI 源码无 rect/control handle；物品身份是 `contracts::QolItemHandle`（slot+generation）。 |
| §4.3 句柄 + generation | `result.h:57,71`；`feature.cpp:363-371` 保存 handle；`feature.cpp:735,792-793` 执行前复核；无跨帧裸指针（`g_make_item_input` 为单次调用临时指针，`:427,1114-1118`）。 |
| §3.3 / DoD-5 无业务互斥量 | 无 `mutex`/`try_lock`；4 个 `std::atomic` 仅作门禁/日志预算（`feature.cpp:121,126`、`ui/feature.cpp:15`、`ui/runtime.cpp:68`）。 |
| §3.1 写命令编组 | 业务写 `write_stuff_need`/`set_stuff_item` 经 `commands.post_typed`（`ui/feature.cpp:205-219`）；同步写端口调用只做 UI 表现（`set_ui_type`/`show_text`/`show_yesno`，`ui/feature.cpp:79-117,248-278`）。 |
| §2.2 业务规则代码化 | 8 条静态配方（`catalog.cpp:86-111`）与 26 类动态配方保留在 C++，正确；仅 seed 来自 `save.facts`。 |

### 1.2 未满足项

| 编号 | 条款 | 事实 |
|---|---|---|
| G-01 | §4.1 / `ARCHITECTURE.md:74,351` | `plugins/custom_recipe/table.cpp:420,426-430,450` 在插件内持有 `extern uintptr_t g_base` 与 5 个十进制 VMA（`3152304/3152312/3152314/3152288/3152296`），等于 `game_symbols.h:414-416,427-428` 的 `G_RECIPEBASE_*`/`G_MIXTUREBASE_*`，同一事实维护两份。`check_offsets.py` 只匹配十六进制与 `VMA/ADDR/OFFSET` 标识符，漏检该十进制形态。 |
| G-02 | §3.2 / DoD-5 | 无 `EntryGuard`/线程断言。主线程契约只写在注释（`feature.cpp:363`、`catalog.cpp:138-141`）。当前风险由「`api_execute` 入口先清 pending」与 generation 校验兜住（`feature.cpp:745-763`），未验证。 |
| G-03 | §6-5 组件抽取 | 18 个文件平铺 + `ui/` 子目录；`feature.cpp` 1,267 行承载结果层、材料需求、动态配方事件、ownership ops、命令 handler。仅可读性问题，不影响依赖方向。 |

## 2. 目标与非目标

### 2.1 目标

1. 插件目录内不再出现游戏地址或 `g_base`；RECIPEBASE/MIXTUREBASE 槽位只有一个权威来源。
2. 表注入入口在 host 与 Android 上编译同一份代码，测试不再依赖 `__ANDROID__` 宏伪造。
3. 目录结构体现 L0/L4/L5/L6 层次，让 `check_layers` 能锁住方向。
4. 所有阶段行为不变：注入字节、状态分类、ownership 语义、日志 source 全部保持。

### 2.2 非目标

- 不改 Host SDK、`QolGameHost`、capability 面、`craft.facts`、HTTP/JNI（保持 0）。
- 不拆 `feature.cpp` 的算法；不引入事务/回滚（CR01 口径不变）。
- 不改 `TableAcquireStatus` 分类语义与自动恢复条件（仅 `kNotReady`）。
- 不新增资源声明、不改 13 条 claim 顺序。

## 3. 阶段 1（必修）：表槽位地址下沉

### 3.1 设计

插件保留全部算法（`table_acquire_core`/`table_deactivate_core`），只把「`.bss` 槽位装配」移出：

```
contracts/game_ports/recipe_table_port.h     新契约（POD）
game/adapter_ports/recipe_table_port.{h,cpp} 唯一地址来源（可 include game_symbols.h）
plugins/custom_recipe/table_runtime.cpp    端口 → TableAccess 适配 + 3 个生产入口
plugins/custom_recipe/table.{h,cpp}        纯核心，删除 #ifdef __ANDROID__ 整段
```

### 3.2 新端口契约

`contracts/game_ports/recipe_table_port.h`：

```cpp
namespace qol::contracts {

// 与插件 TableAccess 字段一一对应。指针只在 game/ 侧由 VMA 装配。
struct QolRecipeTableSlots {
    uintptr_t base;
    void** recipe_ptr_slot;
    void** mixture_ptr_slot;
    uint8_t* recipe_size_slot;
    uint8_t* mixture_size_slot;
    uint16_t* recipe_count_slot;
};

struct QolRecipeTablePort {
    QolVersion abi_version;
    // 全有或全无：base 未就绪时把 6 个字段全部置 0；否则 6 个字段必须同时有效。
    void (*resolve)(QolRecipeTableSlots* out);
};

static_assert(std::is_standard_layout<QolRecipeTablePort>::value,
              "QolRecipeTablePort must remain standard layout");
static_assert(std::is_trivially_copyable<QolRecipeTablePort>::value,
              "QolRecipeTablePort must remain trivially copyable");

}  // namespace qol::contracts
```

`contracts/game_ports/ports.h`：

- `enum class QolPortId` 追加 `QOL_PORT_RECIPE_TABLE = 33`（当前最大 32）。
- 追加 `constexpr QolVersion kQolPortRecipeTableVersion{1, 0};`。

### 3.3 game 侧实现

`game/adapter_ports/recipe_table_port.cpp`（形态参照 `craft_lottery_port.cpp:1-40,81-83`）：

```cpp
#include "game/adapter_ports/recipe_table_port.h"
#include "contracts/game_ports/ports.h"
#include "game/symbols/game_symbols.h"

extern uintptr_t g_base;

namespace qol::game {
namespace {
void resolve(contracts::QolRecipeTableSlots* out) {
    if (out == nullptr) return;
    *out = contracts::QolRecipeTableSlots{};
    if (g_base == 0) return;                       // 与旧行为一致：未就绪不装配
    out->base = g_base;
    out->recipe_ptr_slot   = reinterpret_cast<void**>(g_base + G_RECIPEBASE_DATA_VMA);
    out->mixture_ptr_slot  = reinterpret_cast<void**>(g_base + G_MIXTUREBASE_DATA_VMA);
    out->recipe_size_slot  = reinterpret_cast<uint8_t*>(g_base + G_RECIPEBASE_SIZE_VMA);
    out->mixture_size_slot = reinterpret_cast<uint8_t*>(g_base + G_MIXTUREBASE_SIZE_VMA);
    out->recipe_count_slot = reinterpret_cast<uint16_t*>(g_base + G_RECIPEBASE_COUNT_VMA);
}
}  // namespace

const contracts::QolRecipeTablePort& recipe_table_port() {
    static const contracts::QolRecipeTablePort port{
        contracts::kQolPortRecipeTableVersion, &resolve};
    return port;
}
}  // namespace qol::game
```

刻意**不**加 `game_memory_accessible` 校验：旧实现也不校验，加上会改变 `NotReady/Unsupported` 分类。该加固另立条目（见 §8）。

`module/app/src/main/cpp/CMakeLists.txt` 的 `gamebridge` 源列表（`:263-292` 段）追加一行 `game/adapter_ports/recipe_table_port.cpp`。

### 3.4 注册表注入

`host/plugin_registry.cpp`：

- include 段（`:10`、`:62`）追加 `contracts/game_ports/recipe_table_port.h` 与 `game/adapter_ports/recipe_table_port.h`。
- 端口分发链（`:261-270` 附近）追加：

```cpp
if (id == contracts::QolPortId::QOL_PORT_RECIPE_TABLE) {
    const auto& port = qol::game::recipe_table_port();
    if (out != nullptr) *out = AvailablePort{id, port.abi_version, &port};
    return true;
}
```

Host ABI 与 `QolPortRequirement` 结构不变。

### 3.5 插件接线

`table.h`：

- include `../../contracts/game_ports/recipe_table_port.h`。
- 新增 `void table_bind_port(const contracts::QolRecipeTablePort* port);`
- 保留 `custom_recipe_table_acquire/ensure/deactivate` 与 `set_context` 声明；删除"Android 入口"措辞，改为"经端口装配槽位"。

`table_runtime.cpp`（新，约 90 行）：承接现 `table.cpp` 的 `__ANDROID__` 整段（`:417-493`）中属于适配的部分：

- `g_game`/`g_sdk`、`table_log_line`（**source 字符串 `plugins/custom_recipe/table` 保持不变**）、`set_context`、`g_state`（`TableState`）、`g_dedupe`（`TableLogDedupe`）一并迁入本文件；这些仍是本 TU 的单一实例，语义与 `table.cpp:432-437` 相同。
- `g_table_port` + `table_bind_port`。
- `TableAccess resolve_access()`：`port == nullptr` → 全 0；否则调 `resolve()` 后逐字段映射。
- `custom_recipe_table_acquire()`：`access.base == 0` → `table_status_result(kNotReady)`（与旧 `table.cpp:474` 一致）；否则 `table_acquire_core(...)`。槽位部分为空时由核心返回 `kInternal`（`table.cpp:250-255` 已有）。
- `custom_recipe_table_deactivate()`：`access.base == 0` → no-op（与旧 `:487` 一致）；否则 `table_deactivate_core`。
- `custom_recipe_table_ensure()` 保持 bool 薄包装。

`table.cpp`：删除 `:417` 起的 `#ifdef __ANDROID__ … #endif` 整段（`:417-493`）；文件变为纯核心（`build_record_bytes`/`derive_material_count`/`classify_table_probe`/`table_acquire_core`/`table_deactivate_core`/`table_status_result`），不含任何平台宏。

`plugin.cpp`：

- include `contracts/game_ports/recipe_table_port.h` 与 `table.h`（后者已在）。
- `kPorts`（`:309-317`）追加第三项 `QOL_PORT_RECIPE_TABLE` v1.0；注释"两个写端口"改为"两个写端口 + 一个表槽位端口"。
- `descriptor` 的 `kPorts, 2` → `kPorts, 3`（`:353`）。
- `plugin_install`（`:131`）：`port_count != 2` → `!= 3`；新增 `table_port_ready()`（`abi_version` 满足 + `resolve != nullptr`），失败 `return 1`；在 `set_write_port(recipe_write_port)`（`:220`）之后调用 `::custom_recipe::table_bind_port(table_port)`，与其它 setter 同批生效。
- `plugin_deactivate`（`:230-247`）追加 `table_bind_port(nullptr)`，位置在末尾 `custom_recipe_table_deactivate()` 之后。

`plugins/custom_recipe/CMakeLists.txt`：追加 `table_runtime.cpp`。

### 3.6 测试改造

| 目标 | 变化 |
|---|---|
| `custom_recipe_tests` | 不变（本就只链核心）。 |
| `custom_recipe_table_readiness_tests` | 源列表（`tests/CMakeLists.txt:309-313`）追加 `table_runtime.cpp` 与 `game/adapter_ports/recipe_table_port.cpp`；删除 `:317` 的 `__ANDROID__` 定义；测试去掉自身 5 个 VMA 常量与两处 `#ifdef __ANDROID__`（`g_base` 定义保留在文件作用域，作为 fake `.bss` 基址），改为 `g_base = (uintptr_t)g_fake_bss` 后 `table_bind_port(&qol::game::recipe_table_port())`。断言口径不变（`NotReady`/`Ok`/`Unsupported`/`Busy`/deactivate 保留缓冲）。 |
| `custom_recipe_owner_fixture_tests` | `:1176-1178` 改 `port_count == 3` 并断言 `ports[2].id == QOL_PORT_RECIPE_TABLE`；fixture 的 `ctx.ports` 补第三个假端口；保留本地 `custom_recipe_table_*` 替身与计数，另加空实现 `void table_bind_port(const contracts::QolRecipeTablePort*) {}`。 |
| `custom_recipe_handle_chain_tests` | 不变（不链 `plugin.cpp`，本地已有表替身）。 |

### 3.7 检查器补强

`scripts/maintenance/check_offsets.py` 新增规则：

- 类别 `offsets_plugin_base`：`plugins/` 下源码行（去 `//` 注释后）出现 `\bg_base\b` → 违规。
- 扩展现有裸地址规则：`(?:g_base|image_base)\s*\+\s*[0-9]{5,}`（十进制 VMA）→ 违规，堵住本次漏检形态。

同时新建 `scripts/maintenance/test_check_offsets.py`（unittest，mock `ac.iter_sources` 注入源码字符串，不读真实树、不写临时文件）：正例覆盖 `plugins/x.cpp` 的 `extern uintptr_t g_base;`、`g_base + 3152304`、`bss(vma)` 转发；反例覆盖 `game/adapter_ports/*.cpp` 的同形态（合法）、`// g_base 未就绪` 注释行、`kWh4FnvOffsetBasis` 一类哈希常量（不得误报）。

### 3.8 提交切分

| 提交 | 内容 | 单独可构建 |
|---|---|---|
| 1 | 新契约 + `recipe_table_port.{h,cpp}` + 注册表注入 + 根 CMake | 是（新增未被引用） |
| 2 | `table_runtime.cpp` + `table.cpp` 纯化 + `plugin.cpp` 三处 + `tests` 改造 | 是 |
| 3 | `check_offsets` 规则与单测 | 是 |

## 4. 阶段 2（可选）：目录分层迁移

纯移动 + include/CMake 路径更新，无代码改动，1 个提交（参照 `4d34ff45` core.item 的节奏）。

| 目标路径 | 来源 |
|---|---|
| `model/rules.{h,cpp}`、`model/catalog.{h,cpp}`、`model/result.h` | 根目录同名文件 |
| `result/feature.{h,cpp}` | 根目录 `feature.{h,cpp}`（L1 状态 + L4 用例） |
| `table/table.{h,cpp}`、`table/table_runtime.cpp`、`table/text_table.h` | 根目录同名 |
| `ui/` | 不变 |
| `plugin.cpp`、`plugin.h`、`settings_contribution.*` | 不变 |

include 相对路径影响面：插件内 10 个文件 + 6 个测试文件（`test_custom_recipe.cpp`、`test_custom_recipe_handle_chain.cpp`、`test_custom_recipe_owner_fixture.cpp`、`test_craft_ui_guard.cpp`、`test_craft_ui_handle_identity.cpp`、`test_craft_facts_plugin.cpp`）+ `tests/CMakeLists.txt` 的 4 处源路径。

不在此阶段拆 `feature.cpp`。触发条件：文件超 1,500 行，或同一迭代内三个以上互不相关的改动落在该文件，再单独立项。

## 5. 阶段 3（可选）：主线程/重入契约

不做行为变更，只把隐式约定变可观测：

- 在 `table_runtime.cpp` 与 `feature.cpp` 的 6 个入口（acquire/deactivate/place/prepare/execute/make_item）加**日志级**主线程检查：`g_game != nullptr && g_game->is_main_thread != nullptr && g_game->is_main_thread() == 0` 时写一次 `wrong thread entry` 并继续（不返回错误）。
- 契约写入 `docs/refactor/custom-recipe/refactor-plan.md` §5 与插件参考的机制段，不新增公共头。
- 升级为 fail-closed 的前置条件：确认 `game_host_mark_main_thread()` 在插件 install 前已执行（当前由 host 启动期标记，需真机日志确认）；未确认前不做返回错误。

## 6. 验证矩阵

| 阶段 | 必须执行 | 通过标准 |
|---|---|---|
| 1 提交 1 | `scripts/build-debug.sh`（普通）；`check_arch` | BUILD SUCCESSFUL；层依赖/裸地址 0 违规 |
| 1 提交 2 | 4 个受影响 target 重新构建后运行：`custom_recipe_tests`、`custom_recipe_table_readiness_tests`、`custom_recipe_owner_fixture_tests`、`custom_recipe_handle_chain_tests`；`check_arch`；`check_http_routes`；`scripts/build-debug.sh` | 全 PASS；routes 147 无漂移 |
| 1 提交 2（设备） | 安装 → 进档 → 开关 `customRecipeEnabled` 关/开 → 宝石强化页合成一次、升阶一次 → logcat 查 `recipe table not ready`、`ownership restored`、FATAL | 注入与还原语义不变；无 SIGSEGV；13 资源 live；`VM-CR-MERGE-01` 仍按既有状态登记，本批不改写 |
| 1 提交 3 | `uv run python scripts/maintenance/check_offsets.py --all`；`uv run python scripts/maintenance/test_check_offsets.py`；`check_arch` | 新规则 0 违规、单测全绿；`grep -rn '\bg_base\b' plugins/` 为空 |
| 2 | 全量 host CTest + `check_arch` + `build-debug.sh` | 与阶段 1 结束时逐项一致（纯移动不应改变任何结果） |
| 3 | 受影响 CTest + 构建 + 真机 logcat | 无非主线程入口日志；行为与阶段 2 一致 |

证据目录：`.tmp/custom-recipe-arch/<阶段>/`；结论登记到 `docs/refactor/verification.md` §5 对应小节（不新建 ACCEPTED 结论）。

## 7. 风险与回滚

| 风险 | 影响 | 处置 |
|---|---|---|
| 端口顺序错位 | `ctx.ports[]` 按 descriptor 顺序，位置写错会取到错误结构 | 只在末尾追加；install 逐个校验 ABI + 函数；fixture 断言 `ports[2].id` |
| 端口未注入 | host/旧装配路径缺第三端口 → install 直接失败（fail-closed） | 注册表与 descriptor 同提交落地；kernel-only 构建同样编译 `gamebridge` 源，无分支遗漏 |
| 槽位部分为空 | 核心返回 `kInternal`，分类从 NotReady 变化 | 契约要求全有或全无；游戏侧单点实现，测试覆盖 base==0 与全字段有效两条 |
| 日志 source 变化 | 既有日志检索/验收脚本失配 | `table_runtime.cpp` 保留字符串 `plugins/custom_recipe/table` |
| 移动导致 include 断裂 | 编译失败（host 与 Android 都要过） | 阶段 2 单独提交，仅路径改动；不做格式化与重命名 |

回滚：每个提交独立 revert。阶段 1 提交 1 是可加性变更，revert 无副作用；提交 2 revert 后插件回到 `__ANDROID__` 实现（`table.cpp:417-493` 需随 revert 恢复）。

## 8. 遗留观察（不在本方案内）

- `.bss` 槽位可访问性未校验（`game_memory_accessible`）：现有实现同样未校验，加校验会改变分类，需单独裁决。
- `feature.h:20` 引用的 `ADR-0005` 不存在（`docs/refactor/adr/` 仅 0001–0003）。
- `PrepareOutcome`/`CraftOutcome` 与结果 API 成员表未变；本方案不触碰。

## 9. 完成定义

1. `plugins/custom_recipe` 内 `g_base` 与 VMA 常量命中数为 0（`check_offsets` 新规则 + grep 双重确认）。
2. `game_symbols.h` 的 `G_RECIPEBASE_*`/`G_MIXTUREBASE_*` 是这些槽位的唯一来源。
3. `table.cpp` 不含 `#ifdef __ANDROID__`；host 与 Android 走同一份入口代码。
4. 4 个受影响 CTest 全绿；`check_arch`、`check_http_routes`、`check_offsets` 全绿。
5. `scripts/build-debug.sh` 成功；受影响路径真机 smoke 有日志证据。
6. `docs/reference/plugins/custom_recipe.md` 的 descriptor 面（ports 2→3）、文件地图、层分工同步更新；`docs/INDEX.md` 登记本文档。

## 10. 实施记录（阶段 1）

- **落地内容**：新增 `QOL_PORT_RECIPE_TABLE` v1.0（契约 + game 装配 + registry 注入）；插件侧删除 `extern g_base` 与 5 个十进制 VMA，新增 `table_runtime.cpp` 承接端口映射与 ensure/deactivate；`table.cpp` 变为纯核心，`#ifdef __ANDROID__` 分支删除；descriptor ports 2→3。
- **与设计稿的偏差（均减少外部改动）**：
  1. 根 `CMakeLists.txt` 只需追加 `game/adapter_ports/recipe_table_port.cpp`；插件源由 `plugins/custom_recipe/CMakeLists.txt` 自有 target 收集，根清单经 `QOL_BRIDGE_PLUGIN_SOURCES` 过滤，无需登记 `table_runtime.cpp`。
  2. `recipe_table_port.cpp` 不 include `game/access/game_access.h`（该头依赖 `jni.h`，host 测试目标无该头），改用同形 `extern uintptr_t g_base;` 声明。
  3. readiness 测试不再复制 VMA：链接真实 game 端口，用 fake `.bss` 作 `g_base` 后经 `resolve()` 取槽位地址。
- **验证证据**（`.tmp/custom-recipe-arch/p1/`）：
  - Host：`custom_recipe_tests`、`custom_recipe_handle_chain_tests`、`custom_recipe_table_readiness_tests`、`custom_recipe_owner_fixture_tests` **4/4 PASS**。
  - `check_arch` 全部通过；`check_offsets --all` 违规 0（`plugins/` 无 `g_base` 命中）；`test_check_offsets.py` 9 用例通过。
  - `scripts/build-debug.sh --offline` BUILD SUCCESSFUL：`output/inotia4_qol_lsposed_debug_2610252029_80c40d98bcff.apk`（SHA-256 `80c40d98bcfff45780c3ac4143d2e8397ecbd03ce3fba193f6fbee9fac98808c`）。
- **未完成**：真机 smoke（表注入/还原、开关 toggle、合成与升阶）尚未执行；`VM-CR-MERGE-01` 状态不变。阶段 2/3 未实施。
