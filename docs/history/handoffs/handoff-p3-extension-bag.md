# Handoff：扩展背包 P3 批次 2/3 实施与调试（2026-08-29）

> 供后续会话快速接续。控制面（docs/development/features/extension-bag/control-plane.md）为权威状态源，本文补足
> 控制面未覆盖的调试细节、根因结论与试验中间态。

## 1. 本会话成果总览（提交链）

| 提交 | 内容 |
|---|---|
| 404f3c3 | P1 启动：袋/物品机制逆向（docs/reference/game/bag.md）、frida 探针 |
| 06b42b7 | P1 容量派生契约落地：derive_capacity 替换 kFixedCapacities |
| f3b87de | P1 prepare journal 格式 v1（独立 section + 三方对照裁决） |
| 2f6340c | P1 所有权账本（ownership_ledger.h） |
| 9145bae | P3 批次 1：投影常驻 + 保存门禁 + 二次点击信息态 + 音效 |
| 8f4b7c8 | P3 G-8 拖动路由零干预化（事件参数驱动 drop 路由） |
| 6f55686 | 扩展标签 data[0] 清零（纯切袋崩溃根因修复，后被升级替代） |
| 72b503a | 借出对象不真释放（根治 TouchState/控件残留引用悬空） |
| e103dd5 | 扩展标签放真实背包物品对象（用户方案） |
| 3a13c4c | G-1 标签视觉 + 袋信息面板 v1 |
| bef... | G-9/G-10 标签 ControlItem 化（ControlItem 原语组装 + 自定义 proc） |
| 82c5b88/f3b87de | GetCount 判界 + GetAbsoluteRect 陷阱移除（x8 sret） |
| 417dc47/72b503a | 延迟释放 → 不真释放（悬空根治演进） |
| fc5294a | 投影对象释放前清控件引用 + 底框 w6 修正 |
| 6de40de | 投影期间每帧清 TouchState MOVING_CTRL |
| e103dd5 后 | 标签挂载层试验（depth4 场景根 → depth1 试验中，工作树未提交） |

## 2. 当前进行中（接续点）

**扩展标签挂载层试验**：标签（ControlItem，挂场景根）在面板动画期间随容器漂移。
用户方案：动态计算原版袋绝对位置换算。当前工作树已切 **depth1 挂载试验版**（构建通过、已部署）。

**depth 父链实测**（container_rect_probe.js，CO_PARENT @ +0xa0）：
- depth0 袋容器 (389,12) ← 标签原挂载（0x3049e0+0x50）
- depth1 (496,0) ← **当前试验层**
- depth2 (0,133)
- depth3 (231,0)
- depth4 (0,0) 场景根（rect 恒定）
- depth5 (0,0)
- 累加 = 容器绝对 (1116,145) = 原版袋 0 位置

**验证要点**：面板开合动画期间标签是否仍漂移。depth1 若仍动 → 依次试 depth2/depth3；
若 depth1/2/3 都动（动画层）→ 方案改为**每帧动态重算**（不依赖静态挂载层，draw 时
实时换算 rect）。

**相关代码**：`scene_root_from_container_locked`（game_ui_virtbag.cpp，循环次数=层数）、
install_extension_tab_buttons_locked 内的换算逻辑（bag0_abs - mount_abs）。

## 3. 崩溃根因结论（已闭合，勿重蹈）

### 3.1 纯切袋崩溃（TouchHandle 物品指针污染链）
- **链条**：手指按下命中控件 → TouchHandle 读控件 **data[0] 当"物品指针"** 存入
  TouchState(+0x30 MOVING_CTRL 等) → Scene_Draw/UIEquip_Draw 逐帧画 TouchState 里的物品
  → 指针无效即 SEGV
- **ControlButton 的 data[0] 是按钮数据块指针（非物品）**——扩展标签曾是 ControlButton →
  纯切袋即崩（tombstone_15 等，fault addr 全随机）
- **已修**：标签改 ControlItem（GetUserType==2）+ data[0] 放真实背包物品对象
- **原版为什么不崩**：原版袋标签 data[0] 永远是有效物品或 null（RefreshBagArea 保证）

### 3.2 GetAbsoluteRect 禁止 C++ 直调
- **x8 sret 出参**：真实 UiRect = 4×int64 = 32B；C typedef 16B 结构体走 x0/x1 返回
  （AAPCS64 ≤16B 非 HFA 不用 x8）→ x8 残留垃圾 → GetRelativeRect `stp [x8]` 写只读页
- **代码库早有记录**：game_ui_custom.cpp:74（ctrl_abs_point 手工父链累加模式）、
docs/reference/game/ui.md §6 第 3 坑——本次重蹈后陷阱 typedef 已移除（game_access.h）
- **替代**：手工读 CO_RECT_X/Y + CO_PARENT 累加（纯内存读）

### 3.3 借出对象不真释放
- 借出对象可能被 TouchState/控件 data/desc 面板多处残留引用，ItemPool_Free 后悬空
- **根治**：free_module_object_locked 改为仅清模块引用（data[0] 置 nullptr）+ defer
  no-op，对象内存保留至进程结束（单个 ~48B，开发期可接受；P4 对象桥接后此机制退役）
- 释放前投影控件同步 SetItem(nullptr)（Scene_Draw 不画悬空）

## 4. TouchHandle 事件模型（实证）

### 4.1 控件 proc 事件码（UIEquip_InvenItemControlEventProc 0xb911c）
| event | 行为 |
|---|---|
| 0x80 | **desc_type=2 + MakeDesc**（详情面板，一次点击即出） |
| 0x02 | SetMoving(0)/drop 到物品控件（INVEN_MoveItem 移动/堆叠） |
| 0x04 | ApplyStuff 或 drop 到袋（INVEN_SaveItemOnEmpty） |
| 0x01/0x10 | UIDesc_IsOn()==0 → MakeDesc；已开 → SetOn(1) |
| 0x20 | SetOn(0) 取消选中 |
| 0x81 | press 确认（返回 1 建 moving） |

### 4.2 panel proc 事件码（TouchHandle → 面板，virtual_bag_event 拦截层）
- 0x17 = press、0x18 = release、0x19 = move（**面板级触摸移动，非物品拖动专属**——
  点袋标签也产生 0x19）
- 0xffffffff800000XX = 系统事件（触摸屏原始）

### 4.3 TouchState（G_TOUCH_STATE_VMA = 0x301cf8）
- +0x10/+0x28/+0x48：TouchHandle_Event 写入的各字段（press 时）
- +0x30 = MOVING_CTRL（拖动中控件——**每帧清此字段曾致点击全灭**，press/drop 字段不能动）
- +0x58 = drop source ctrl
- +0x60/+0x68 = release 坐标

### 4.4 袋标签原版交互语义（用户规格）
- 袋标签一次点击 = 切换到该袋；二次点击 = 袋信息面板（含解除按钮）
- 解除：有物品弹窗拒绝；空袋解除放回背包空位
- 物品一次点击 = 选中 + 信息详情（无二次点击概念）

## 5. 遗留待办（优先级序）

1. **挂载层试验收尾**：depth1 部署验证中；漂移仍在则 depth2/3 或每帧动态重算
2. **金色选中框**：TouchHandle 高亮跑到扩展标签上（原版袋标签无此框）——
   hook ControlItem_Draw 入口按控件类型过滤可定位绘制者
3. **袋信息页原版化**：标签已 ControlItem 化 + data[0]=袋物品对象——
   **原版 MakeDesc 详情**（信息+解除按钮）理论上可直接触发（袋标签语义 =
   UIEquip_InvenBagControlEventProc 的 0x80/0x01 分支）——
   extension_tab_item_proc 加 0x80/0x01 → MakeDesc 转发即可验证
4. **drawdiag 诊断日志**：定位完成后移除（draw_inven_item_wrapper 内，快照比对限频）
5. **控制面更新**：v1.14 后缺批次 2/3 成果与崩溃序列登记
6. **G-8 拖动**：TouchHandle 拖动建立被 item proc 0x81 拒（返回 0）——
   深挖 0x81 协议或恢复模块状态机（用户挂起中）
7. **G-9 挂载点收尾**：item proc 0x04 加 GetItemSlotIndex>=16 防护已做；
   GetBagSlotIndex 语义冲突（循环上限 6）处理待定
8. **未装备袋 item proc 放行**：0x81 对未装备袋标签的处理待验证

## 6. 工具与探针（scripts/analyze/）

| 脚本 | 用途 |
|---|---|
| container_rect_probe.js | 控件父链 rect 累加（CO_PARENT @ +0xa0） |
| dump_projection_rects.js | 投影控件 rect dump（frida x8 限制，参考用） |
| desc_state_probe.js | desc_type/当前袋实时监控 |
| drag_probe.js | SetMoving/INVEN_MoveItem/itemProc hook |
| draw_part_probe.js | GRPX_DrawPart 全量记录（标签区过滤） |
| porting_probe.js | ITEM_DrawPorting item/lr/in_view 记录 |
| touch_automation.py | 触摸注入/检测（坐标自动换算，代理可直驱） |

## 7. 调试方法论（本会话验证有效）

- **DCG 误判规避**：objdump 用 `--disassemble` 长旗标；重定向到 .tmp/ 用 python 写文件
- **frida x8 限制**：结构体 sret 返回的函数无法从 frida 正确调用（x8 不可控）——
  读值用 Interceptor.attach，不调 NativeFunction
- **崩溃定位三板斧**：tombstone backtrace 符号化（llvm-nm 查 libgamebridge.so）→
  drawdiag 限频日志（快照比对）→ Interceptor 探针实证
- **用户视觉校准法**：UI 改动以"历史版本对照"收集用户反馈（loc 编号变更记录在案）
- **真机触摸注入**：touch_automation.py 可代理直驱（用户授权），坐标自动换算

## 8. 关键文件

- module/app/src/main/cpp/feature/extension_bag/game_ui_virtbag.cpp — 核心改动（标签/投影/门禁/路由）
- module/app/src/main/cpp/game_patch.cpp — item proc wrapper（drop 路由/事件日志）
- module/app/src/main/cpp/feature/extension_bag/model/virtual_bag_state.h — 状态模型（info_bag/unequip/journal）
- module/app/src/main/cpp/game_access.h/.cpp — 符号解析（陷阱已标注）
- docs/reference/game/bag.md — 背包/袋对象机制逆向记录
- docs/development/features/extension-bag/control-plane.md — 控制面（权威状态）
- apk/decoded/lib/arm64-v8a/libgame.so — 反汇编目标（.tmp/game_full.asm 全量转储）
