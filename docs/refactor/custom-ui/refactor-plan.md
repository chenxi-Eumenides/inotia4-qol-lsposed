# `core.custom_ui` 局部拆分方案

日期：2026-10-25。状态：变更 A/B0/B 已实施；变更 C 未执行，只保留后续方案。
依据：`docs/refactor/plugin-architecture-guide.md` 的 §1 层内聚、§3.2 重入、§7 反模式清单；不做 L0–L6 全量重构（理由见 §8）。

## 0. 前提

- 插件功能与画面当前没有问题。视觉验收由用户真机目视判定完成，**不能用 Host 测试替代**，本方案也不重新定义任何验收卡状态。
- 本方案只做结构移动与构建门修正：不改绘制参数、控件几何、调用顺序、错误码、capability 版本、Adapter 槽位和 HTTP 路由。
- 现有自动化门槛：`check_arch.py` 四类校验 0 违规；`core.custom_ui` 相关 Host target 全绿。
- 构建范围：本轮只做 Debug 构建与 Host 测试，不执行 `scripts/build-release.sh`；Release 侧排除以 CMake 源列表和 `#if` 静态复核为准，不报告 Release 产物级验证。
- 改动边界：写范围限定在 `plugins/core.custom_ui/`。`tests/CMakeLists.txt` 和 `docs/` 属插件外，具体改动行见 §5、§10，需先经用户批准再动。

## 1. 目标与非目标

目标：

1. Debug 面板不再进入 Release 二进制（当前 1079 行代码在所有构建配置下都会编译）。
2. 形状栅格化从 2821 行的组件文件剥离成纯几何 TU，组件文件只留注册表、生命周期与原生控件桥接。
3. （可选、暂缓）确认框的 owner 状态机与 overlay 补绘/输入门禁分开。

非目标：L0–L6 目录重排、把 14 种组件改成变体体系、删除 mutex、把栅格化下沉到 Adapter 或 `game/access`。

## 2. 冻结面

| 面 | 位置 | 约束 |
|---|---|---|
| capability 版本 | `docs/reference/plugins/core.custom_ui.md` §2 五项 + `QolUiAdapter` minor | 数值不变，不增删槽位 |
| 组件槽位表 | `ui_components.cpp:2688-2769`（`kUiComponents`） | 顺序、签名、数量逐字不变 |
| 路由事实 | `plugin.cpp:165-178`、`routes.inc` | 3 条 Debug command 路由不变 |
| 视觉与几何常量 | `ui_components.cpp:2183-2184`（半径 256 / 直径 512）、alpha 0–100、开关轨道几何 | 数值与绘制顺序不变 |
| 线程契约 | UI 主线程调用、adapter 一律锁外调用 | 不变 |

## 3. 变更 A：Debug 面板编译门

**现状**：`CMakeLists.txt:13-15` 只把 `panels/debug_scroll.cpp`、`panels/debug_help.cpp` 放进 Debug-only `target_sources`；`panels/debug_components.cpp`（1079 行）写在 `:8` 的公共源列表里，所有构建配置都会编译。而 `plugin.cpp` 对它的 8 处引用全部在 `#if defined(QOL_DEBUG_COMPONENTS) && QOL_DEBUG_COMPONENTS` 内（`:169`、`:185`、`:229`、`:252`、`:277`、`:289`、`:312`、`:331`；对应 `#if` 块 `:165/181/228/249/276/288/311/328`），Release 下不可达。

**改动**：从 `:8` 删除 `panels/debug_components.cpp`，加入 Debug 分支：

```cmake
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_sources(qol_plugin_core.core.custom_ui PRIVATE
        panels/debug_scroll.cpp
        panels/debug_help.cpp
        panels/debug_components.cpp)
endif()
```

沿用现有 `CMAKE_BUILD_TYPE STREQUAL "Debug"` 判定，不改成 `CONFIG` 生成式；两者在既有 `debug_scroll`/`debug_help` 上已被这样使用，本方案不改变该语义。

**影响**：Debug 产物不变；Release 少 1079 行及其调用链常量。`tests/CMakeLists.txt:1663-1666` 的 `debug_two_column_tests` 显式编译该文件，不受影响。

**风险**：若存在未被 `#if` 覆盖的引用，Release 构建期即链接失败（fail-fast）。实施前用 `grep -rn 'debug_components_' --include='*.cpp' --include='*.h'` 复核引用集合。

## 4. 变更 B：形状绘制抽离

**现状**：`ui_components.cpp:2183-2410` 是纯几何/扫描线栅格化（约 228 行），只被 `kUiComponents` 尾部 3 个槽引用（`:2765-2767`）；同一文件还承载注册表（`:44-99`）、创建/销毁生命周期（`:172-660`）、锚定桥（`:1215-1547`）与其余控件绘制。

**新增文件**：

| 文件 | 内容 |
|---|---|
| `ui_geometry.h` | `inline bool rect_within_int32(UiRect)`：从 `ui_components.cpp:2141` 移入并删除 `:108` 的前置声明；两个 TU 共用，9 个既有调用点（`:701/850/1007/1101/1602/1612/1618/1645/2416`）改为直接使用 |
| `ui_adapter_check.h` | `inline bool ui_adapter_ready(const QolUiAdapter*)`、`inline bool ui_adapter_on_main_thread(const QolUiAdapter*)`，等价于当前 `adapter_ready()` / `on_main_thread()`（`:149-165`）；`kUiAdapterBaseMinor` 只保留一处定义（当前 `:120`） |
| `ui_shapes.h` | `QolResult ui_shape_draw_rect(const contracts::QolUiAdapter*, const contracts::UiShapeDesc*)` 及 `_rounded_rect`、`_circle` 两个同形声明 |
| `ui_shapes.cpp` | 搬迁后的几何实现，不持有全局状态，adapter 由参数传入 |

**搬迁清单**（函数 → 现行号，结束于 2410）：

- `kMaxRasterRadius` / `kMaxCircleDiameter` `:2183-2184`
- `valid_shape` `:2186`
- `shape_fill` `:2192`
- `integer_sqrt` `:2198`
- `rounded_corner_inset` `:2209`
- `rounded_row_span` `:2222`
- `circle_row_span` `:2233`
- `draw_circle_pixels` `:2251`
- `draw_rect_shape` `:2261`
- `draw_rounded_rect_shape` `:2283`
- `draw_circle_shape` `:2374`

**`ui_components.cpp` 保留**：3 个同名 trampoline 维持槽位签名，委托给新 TU：

```cpp
QolResult draw_rect_shape(const contracts::UiShapeDesc* desc) {
    return ui_shape_draw_rect(g_adapter, desc);
}
```

**搬迁纪律**：纯移动。不重排扫描线循环、不改 alpha/颜色/半径上限、不合并像素写入、注释随函数一起移动。`shape_fill` 的 `fill_rect_alpha` 空指针检查与 `NotReady` 语义逐字保留。

**测试装配**：`tests/CMakeLists.txt` 现有 7 个 target 显式列 `ui_components.cpp`（`:444`、`:1632`、`:1643`、`:1665`、`:1680`、`:1691`、`:1712`），新 TU 必须同时进入这些 target。做法见变更 B0。

## 5. 变更 B0（前置，无行为变化）：测试源清单收口

`tests/CMakeLists.txt` 在 `CPP_DIR` 定义后新增变量，替换 7 处重复行：

```cmake
set(CORE_CUSTOM_UI_COMPONENTS_SOURCES
    ${CPP_DIR}/plugins/core.custom_ui/ui_components.cpp)
```

B0 只做变量收口（内容等于当前文件集合），保证单独提交即可构建；变更 B 再在变量里追加 `ui_shapes.cpp` 一行。这样后续 B/C 不会漏 target。

## 6. 变更 C（本批不执行，仅保留方案）：确认框 overlay 分离

**现状**：`ui_confirmation.cpp` 974 行、48 个锁点，混合三件事：

- owner/token 状态机与 capability API：`:285-560`、`:700-857`
- overlay scope 登记与帧补绘：`:134-283`、`:875-913`
- 模态输入门禁与返回抑制：`:914-974`

**若执行，边界如下**：

- `ui_confirmation.cpp`：owner 状态机、`open/active/close`、`bind/unbind/installed`。
- `ui_confirmation_overlay.cpp`：overlay scope owner 表、frame overlay 注册与分派、模态门禁、返回抑制。
- 共享状态经窄内部头 `ui_confirmation_shared.h` 暴露**访问器**（adapter 快照、owner 门禁快照、token 状态、`drawn_once`/`v1_overlay` 读写），不暴露变量；每个访问器沿用现有锁范围，adapter 调用仍在锁外。
- 8 个 target 需追加该 TU（`:447`、`:1206`、`:1219`、`:1230`、`:1242`、`:1559`、`:2105`、`:2165`）。

**本批不执行**。48 个锁点跨 TU 后共享状态可见性变大，收益是文件可读性，风险是补绘与门禁的竞态；而视觉回归只能靠用户目视，回归成本高。进入条件：A、B 已合并且用户明确提出；单独提交，不夹带其他改动。

## 7. 提交切分与验证

| # | 提交 | 内容 | 验证 | 通过标准 |
|---:|---|---|---|---|
| 1 | `build(core.custom_ui): gate debug panels to Debug builds` | 变更 A | `scripts/build-debug.sh --offline`；`ctest -R '^debug_two_column_tests$'`；`uv run python scripts/maintenance/check_arch.py`；`git diff --check` | 构建成功、1/1 PASS、0 违规 |
| 2 | `test: centralize core.custom_ui source lists` | 变更 B0 | Host 全量配置 + 受影响 7 target | 全绿，无新警告 |
| 3 | `refactor(core.custom_ui): move shape rasterization into ui_shapes` | 变更 B | 7 个 target + `core_ui_components_tests` + `check_arch.py` + `build-debug.sh --offline` | 全绿、构建成功、0 违规 |

受影响 Host target 固定集合：`core_ui_components_tests`、`native_choice_tests`、`close_button_component_tests`、`nav_button_component_tests`、`debug_two_column_tests`、`native_scroll_list_tests`、`help_page_tests`。

Host 构建目录与目标（每个任务独立目录，符合 `.tmp/` 规则）：

```bash
cmake -S module/app/src/main/cpp/tests -B .tmp/custom-ui-split/host -DCMAKE_BUILD_TYPE=Debug
cmake --build .tmp/custom-ui-split/host -j2 --target core_ui_components_tests native_choice_tests \
  close_button_component_tests nav_button_component_tests debug_two_column_tests \
  native_scroll_list_tests help_page_tests
ctest --test-dir .tmp/custom-ui-split/host -R '^(core_ui_components_tests|native_choice_tests|close_button_component_tests|nav_button_component_tests|debug_two_column_tests|native_scroll_list_tests|help_page_tests)$'
```

Android 侧只允许 `scripts/build-debug.sh --offline`；不直接调用 Gradle。本轮明确不执行 `scripts/build-release.sh`，因此 Release 排除只以 CMake 源列表与 `#if` 静态复核为证，不报告 Release 产物级验证通过。

**真机与视觉**：A、B 不改变渲染路径。部署一次 Debug 包，由用户目视对比 5 个画面：设置页入口按钮、help 双栏、Debug Components 的矩形/圆角矩形/圆形示例、原生选择框、确认框面板补绘。判定口径为"与改动前无差异"。该判定只能由用户完成，Host 测试不作为视觉证据；本方案不改写 `docs/refactor/verification/` 中的既有验收状态，收口时如需更新由用户确认后登记。

### 7.1 实施记录（2026-10-25）

| 提交 | 内容 |
|---|---|
| `d4515727` | `test: centralize core.custom_ui component source list`（变更 B0） |
| `671ecdb9` | `build(core.custom_ui): gate debug panels to Debug builds`（变更 A） |
| `d5d71316` | `refactor(core.custom_ui): move shape rasterization into ui_shapes`（变更 B） |

验证：Host 7 target 编译通过、ctest 7/7 PASS；`check_arch.py` 四类校验 0 违规；`git diff --check` clean；`scripts/build-debug.sh --offline` 成功（产物 `output/inotia4_qol_lsposed_debug_2610252017_bb87996010c9.apk`，SHA-256 `bb87996010c9f2f6d4a3ae7855fddda7f547ba95e909321e1bfdbc5ad143945e`）。未执行 Release 构建，未做真机视觉对比。

## 8. 为什么不做 L0–L6 重构

- 插件不拥有业务数据、持久化或事务，指南 L2/L3/L4 没有对象；§6 迁移 playbook 的查询快照化、命令编组、用例层三步同样无对象。
- 对外是版本化 capability 加 Adapter 尾追加槽，目录重排只换来可读性，且会与未变更的 ABI 语义脱节。
- 现状 `check_arch.py` 全绿、无裸偏移、无 include 环；插件内唯一跨模块耦合是 `ui_components.cpp` → `panels/stack.h` 的回调探针（`stack.h:31-35`，消费点 `stack.cpp:242`），已是窄接口，不需要拆目录。
- 两个 mutex（`feature.cpp:24`、`ui_confirmation.cpp:18`）保护的是 hook/生命周期与能力调用共享的状态，adapter 与 port 调用均在锁外；不属于指南 §3.3 的持锁回调，删除会引入真实竞态。

## 9. 回滚

每个提交独立 revert。无数据迁移、无 ABI 变更、无配置或存档变更；回滚后行为与当前一致。

## 10. 文档同步（插件外，需批准后执行）

- A/B 完成后更新 `docs/reference/plugins/core.custom_ui.md` §7 文件地图（`ui_shapes.*`、`ui_geometry.h`、`ui_adapter_check.h`）。
- 变更 C 的 overlay 文件职责不在本批写入。
- 本文件已登记到 `docs/INDEX.md` 的 `refactor/` 清单。
