# UI 实验记录（v0.6.8 自定义商店面板）

> 配套 `../../reference/game/ui.md`。本文记录自定义面板（`game_ui_custom.cpp`，exp6）的
> 逆向结论与真机调试经验，按时间顺序追加。

## 1. 背景：自定义商店面板

- 目标：替换商店返回按钮 → 打开完全自定义页面（两列多行，每格=按钮+说明，样式用原版）
- 当前进展：按钮色块已显示（v22，3 格：写日志 / 堆叠上限:关 / 关闭面板），**文字未显示**
- 注入链：`/api/debug/custom/inject`（替换商店返回按钮 ExecuteProc + 注入 state 21=INAP_GEMSHOP）
  → `/api/debug/custom/open`（`POPUPSTATE_Push` 调 `custom_panel_enter`）
- 面板绘制：`custom_panel_process`（每帧被 POPUPSTATE_Process 调用）

## 2. 绘制调用链（反汇编实证，lib_20260810）

### UI_DrawStringHAlign @0xaf02c —— 真实签名与内部流程

```
签名：UI_DrawStringHAlign(text=x0, x=x1, y=x2, font=x3, align=x4)
  mov x5, x0            ; x5 = text
  mov x0, x1            ; x0 = x
  mov x1, x2            ; x1 = y
  mov x2, x5            ; x2 = text
  ldr x5, [0x2f3000+0xf88]  ; x5 = 字体全局槽指向的对象（GOT 槽，解引用）
  str x3, [x5]          ; ★ font 参数写入该对象第一个字段（不是直接传参！）
  mov x3, x4            ; x3 = align
  mov x4, #4            ; x4 = 4（固定）
  jmp MW_Graphic_DrawString(x, y, text, align, 4)
```

**关键**：font 不是直接传给底层，而是**写入 `[*(g_base+0x2f3000+0xf88)]`**。
调用前必须保证该槽已指向有效对象，否则 `str x3, [x5]` 直接 SIGSEGV（当前未崩 = 槽已初始化）。

### MW_Graphic_DrawString @0xa24cc

```
签名：MW_Graphic_DrawString(x=x0, y=x1, text=x2, align=x3, mode=x4)
  cmp x4, #2 ; eq → y-6
  cmp x4, #3 ; eq → y-12
  其他 → y 不变
  最终 jmp 0xa7404(text, x, y_adj, align)
```

### 0xa7404（MW_Graphic_DrawString 下层）

```
  bl 0x8b730            ; 返回 x0（推测 = 字体/纹理相关对象）
  jmp 0xa72ec(text, x, y, align)
```

### 官方调用范例：UIWorldMap_Draw @0xd3684

```
  ldr x22, [x0, #0x628]     ; x0=UIWorldMap 对象，+0x628 = 字体配置对象指针
  ldrsw x3, [x22, #4]       ; font id = 字体配置对象 +4
  bl 0xaf02c                ; UI_DrawStringHAlign(text, x, y, font, align)
```

**注意**：font 来自**对象 +0x628 → +4**（UIWorldMap 实例字段），不是全局固定值。
不同面板的字体配置对象不同，且该对象由各面板初始化。

## 3. 当前实现 vs 官方模式的差异（文字不显示疑点）

| 项目 | 官方（UIWorldMap） | 当前 game_ui_custom.cpp |
|---|---|---|
| font 来源 | UIWorldMap 对象 `[+0x628]+4` | `font_id()`：`*(g_base+0x2f3628)+4`（GOT 槽 0x2f3000+0x628 解引用） |
| 调用 | `UI_DrawStringHAlign(text,x,y,font,align)` | 同 |
| 绘制位置 | 完整 Scene_Draw（GRPX_Start/End 间） | process 内 LCD 结构 + GRPX_Start/End |

疑点：
1. `font_id()` 读的是 **GOT 全局槽 0x2f3000+0x628**，而官方读的是**面板对象+0x628**。
   0x2f3000+0x628 槽是否真的存字体配置指针、值是否有效**未验证**。
2. 文字绘制对 font 有效性敏感：font 无效时可能静默跳过（无崩溃）。
3. 下一步：日志打印 `font_id()` 实际值 + `*(g_base+0x2f3000+0xf88)` 是否非空；

## 3b. v23/v24 追加反汇编（2026-08-17）

### UI_DrawStringHAlign 完整签名链（0xaf02c → 0xa24cc → 0xa7404 → 0xa72ec）

```
UI_DrawStringHAlign(text=x0, x=x1, y=x2, font=x3, align=x4) @0xaf02c
  x5 = *(g_base+0x2f3000+0xf88)   # 字体对象指针（全局槽，解引用）
  str x3, [x5]                     # ★ font 参数写入字体对象首字段（int32）
  x3 = x4(align); x4 = #4
  jmp MW_Graphic_DrawString(x, y, text, align, 4) @0xa24cc
```

```
MW_Graphic_DrawString(x=x0, y=x1, text=x2, align=x3, mode=x4) @0xa24cc
  x4==#2 → y 用 y-6；x4==#3 → y 用 y-12；否则 y 原值
  x0=text; w1=x; w2=y(调整后); jmp 0xa7404 @0xa7404
```

```
0xa7404 (text=x0, x=w1, y=w2, align=w3)
  w4=y; w5=align; x19=x; x20=text
  bl 0x8b730          # PLT：GOT 槽 0x2f3000+0xd0 的函数指针（取字体相关对象）
  x2=返回值; x0=text; x1=#0; w3=x; x4=y; x5=align
  jmp 0xa72ec @0xa72ec
```

**结论**：font 参数最终由 `UI_DrawStringHAlign` 写入 `[*(0x2f3000+0xf88)]` 对象首字段，
后续 `MW_Graphic_DrawString` 通过 `0x8b730`（GOT 槽 0x2f3000+0xd0）取字体对象。
→ 只要传给 UI_DrawStringHAlign 的 font id 有效即可，不要求 GOT 槽 0x628 语义。

### Scene_Draw_POPUP_SC_STORE 绘制前清字体槽（0x14f5a0 完整路径）

```
0x14f5e8: ldr x4, [x4, #0xf88]     # x4 = 字体对象指针
0x14f5f4: str xzr, [x4]            # ★ 官方绘制面板前先把字体对象首字段清 0
```

官方模式：每帧绘制前字体首字段=0，各绘制函数内部自行设置有效 font → 文字显示。
若 DrawProc 内直接调 `UI_DrawStringHAlign` 且不先设字体，可能字体=0 无效静默不画。

### ControlButton_Draw（0xaac2c，72B）确认

```
cbz x0 → ret；Show(!=0x31)；GetData(0x9df18) 非空；[data+0x60]=DrawProc 非空 → blr DrawProc(x0=ctrl)
```

文字不绘制时先检查：btn_draw 是否被调（v22 已确认被调）→ DrawProc 内 SetFontColor+DrawString 参数。

### v23 崩溃教训（SIGSEGV fault addr 0x3c）

- 现象：`Fatal signal 11 (SIGSEGV), fault addr 0x3c`，GLThread 84，backtrace
  `custom_btn_draw ← ControlButton_Draw+56 ← libgamebridge ← POPUPSTATE_Process+32`
- 根因：调试日志里 `g_base+0x307000+0x3e0` 当 UIStore **实例**解引用 `+0x628`，
  但 `0x307000+0x3e0` 是**固定控件槽**（存指针），不是对象实例；槽值=垃圾 → 读 `+0x628` 越界 → 崩。
- 教训：**G_UISTORE_VMA(0x308000) 系列是控件槽，UIStore 实例在 0x307000+0x3e0 槽内存储**；
  访问前必须确认槽值有效（非 0、指向 .data/.bss 范围内），调试读内存优先加边界检查。
- 修复：移除 store_obj+0x628 读取，只保留 font_id() 与 f88 槽日志（v24 已构建未部署）。
   或直接复用**当前商店面板**的字体配置对象（商店面板一定已初始化字体）。

## 4. 已确认的绘制规则（真机实测，勿再踩坑）

1. **GRPX_FillRectAlpha alpha>0x64(100) 直接 return**（0x8fcd0 反汇编）→ 用 `GRPX_FillRect`
   （0x8fb30，alpha 嵌 ABGR 色值）。
2. **颜色 = ABGR**：`0xFFFF0000` 显示蓝，`0xFF0000FF` 显示红。
3. **POPUPSTATE_Process（MainProcess 逻辑阶段）内必须模拟官方 Scene_Draw LCD 结构**：
   `RefreshLCDFlag==1 → GRP_SaveLCD(0xa666c)+UI_SetRefreshLCDFlag(0)`；`==0 → GRP_RestoreLCD(0xa66ac)`。
   之后 `GRPX_Start(0x8f2fc)` → 绘制 → `GRPX_End(0x8f314)`。缺 LCD 结构则 GRPX 绘制不可见。
4. **GRPX_DrawPart（贴图）在 MainProcess 阶段调用 SIGSEGV**（GL 纹理状态未就绪）→ 降级色块。
5. **自定义 root 控件**：`ControlObject_Create` 独立创建 → 触摸命中自实现
   （不转发游戏触摸链，否则 `TouchHandle_SetSelectedControl` → `ControlButton_ControlEventProc` 因
   root 无数据块 `[ctrl+0x50]=null` 读 `[data+0x70]` SIGSEGV；`TouchHandle_DeleteControl` 遍历
   父链 SIGBUS）。
6. **ControlButton_Create（0xaa710）**：初始化数据槽 0x78B，`[data+0x20]=executeProc`、
   `[data+0x30/0x38]=-1`、`[data+0x68]=0`、`[data+0x69]=1`。
7. **触摸事件 0x17/0x18/0x19**：param = 24B `{i64 x, i64 y, i64 id}`，坐标为绝对坐标。
8. **按钮字体色**：`GRPX_SetFontColor(0xFFFFFFFF)`（ABGR 白）。

## 5. 反汇编命令备忘

```bash
# capstone 临时环境（uv run，不装系统包）
uv run --with capstone python3 -c "
from capstone import *
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
f = open('/tmp/opencode/versions/lib_20260810/libgame.so','rb')
f.seek(BASE); code = f.read(LEN)
for i in md.disasm(code, BASE):
    print('0x%x: %s %s' % (i.address, i.mnemonic, i.op_str))
"
```

## 6. 待办（后续轮次）

- [ ] 验证 `font_id()` 值有效性 / 改用商店面板字体配置对象 → 文字显示
- [ ] 触摸命中真机验证（cell0/1/2 回调触发）
- [ ] cell1 堆叠上限开关功能验证（`set_stack_limit_enabled` 翻转 + 文本刷新）
- [ ] 还原按钮样式为原版贴图（GRPX_DrawPart 需 GL 帧内绘制，或改走完整 Scene_Draw 路径）
