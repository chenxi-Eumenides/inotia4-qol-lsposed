# Native hook 引擎与依赖决策

> 状态：CURRENT（决策记录）
>
> 本文记录「入口 hook 用什么原语」的现状、事故证据、候选库对比与现行结论。
> 本文取代 `docs/archive/stage-2-static-support-matrix.md:15`（“不引入 Dobby、ShadowHook 或
> 手写 ARM64 trampoline”）与 `docs/archive/native-inventory-hook-development.md:174`
> （“模块不需要自己实现 ARM64 trampoline”）的结论；archive 原文保留作为历史记录，不改写。

## 1. 结论

1. 入口 hook 继续使用 LSPosed/LSPatch 注入的 `hook_func`（Dobby），**只对入口第一条指令是
   无条件 `b` 的跳板入口**改用模块自研的 4 字节短补丁 `patch/short_hook`。
2. **暂不引入 ShadowHook / Dobby / SandHook / Whale 等第三方 hook 库**。
3. 若后续需要「对非 `b` 入口也做 4 字节补丁」，或 Android 16/17 真机出现 BTI/MTE 相关问题，
   优先评估 **ShadowHook**（MIT，默认 4 字节 + 近址 island，失败报错而非扩大补丁）。

## 2. 事故与证据

monster 改版把部分函数入口改写成 `b <隐藏段桩>`。以 `CHARSYSTEM_DropItem` 为例：

```
f4d30:  b    1b58f8          ← 符号入口
1b58f8: cbz  x0, 1b595c      ← 原函数体入口
1b58fc: b    74f00c          ← 跳到 mod 隐藏段桩 557
1b5900: cmp  w9, #1          ← 桩返回落点，距函数体入口仅 +8
74f024: b    1b5900          ← 桩结束跳回
```

LSPosed 注入的 Dobby 在目标与替换入口超出 ±128MB 时写 12 字节 `adrp/add/br`
（实测设备字节：`900040d1 91120231 d61f0220`），覆盖 `0x1b5900`。mod 桩返回后执行补丁
字面量，GL 线程跑飞（100% CPU、单帧栈、无崩溃日志）。

关键结论：**Dobby 不给 4 字节补丁选项**。把替换入口换成 ±128MB 内的近址 thunk 后，
实测仍然写 12 字节，说明其补丁宽度与距离无关。

## 3. 现行实现

- `game/access/symbols.cpp`：不再跟随入口的 `b` 跳板，返回符号入口本身；
  `out_followed` 仅上报“入口是 b 跳板”。
- `patch/short_hook`：近址槽 + `b`（4 字节）+ `ldr x16,#8; br x16` thunk + 单指令
  trampoline；还原时写回原字。只对 `b` 入口启用，重定位对象恒为一条 `b`。
- `hooks/install`：通过宿主注入的 `ShortHookOps` 使用短补丁；`hooks/` 不依赖 `patch/`
  （ARCHITECTURE §3.2 依赖矩阵）。
- `hooks/transaction`：入口安装统一委托 `install_claim_entry_hook`，不再绕开短补丁路径。
- **opt-in 跟随（`ResourceClaim.follow_trampoline`）**：仅当入口是 `b` 且目标首条不是短桩类
  控制流（`b`/`bl`/`b.cond`/`cbz`/`tbz`/`br`/`blr`/`ret`）、目标可读可执行且与入口相距
  16 字节以上时，安装到 `b` 的目标而不是入口。用于「入口被外部周期性还原、目标稳定」的
  变体（当前只有 `world_tuning.hook:CHAR_AddDamage`）。其余入口保持 4 字节短补丁。
  `game::resolve_hook_entry_follow` 由 `HostDeps`/`TxDeps` 注入；未注入或条件不满足时
  回退 `resolve_hook_entry`，语义与既有路径一致。

失败语义：短补丁分配不到近址槽或写入失败时安装失败（fail-closed），不回落到 12/16 字节。

## 4. 候选库对比

| 库 | 许可证 | 维护状态 | arm64 默认补丁 | 近址 island | 失败语义 |
|---|---|---|---|---|---|
| 自研 `short_hook` | 本仓 | 当前 | **4B `b`** | 自建近址槽 | fail-closed |
| **ShadowHook** | MIT | 活跃（2026-10） | **4B `b`** | 默认开启 | 报错（errno 40/41/42） |
| Dobby | Apache-2.0 | 2024-03 后停滞 | 12B / 16B | 需 `dobby_set_near_trampoline(true)` | 静默回落到 12/16B |
| SandHook | Anti-996（非 OSI） | 2021-06 | 16B 或 `HVC`+SIGILL | 无 | — |
| Whale | Apache-2.0 | 2020-12 | `Mov`+`Br`（>4B） | 无 | — |
| bhook / xhook | MIT | 活跃 | PLT/GOT hook | 不适用 | 不适用（改不了函数入口） |

要点：

- ShadowHook v2.0.0 起 release 版禁用 16 字节方案，只保留 4 字节跳转；island 分配失败报错。
- Dobby 的 near 模式默认关闭，且分配失败会静默回落到 12/16 字节——正是本次事故的形态。
- SandHook 的 Anti-996 非 OSI 许可证，Whale 与 SandHook 均已长期停更，排除。

## 5. 为什么暂不引入

1. 现行短补丁只处理 `b` 入口，重定位对象是一条 `b`，不存在指令解码风险；第三方库的完整
   重定位能力用不上。
2. 引入需要额外处理初始化、16KB 页、BTI/MTE、与 LSPatch 自带 Dobby 共存、信号链顺序，
   增加新的不确定性。
3. 现有实现在真机完成写入、还原、重复安装与实战验证；host 测试覆盖安装/事务路径。

## 6. 重新评估的触发条件

满足任一条即重新评估，优先 ShadowHook：

1. 需要对非 `b` 入口（普通函数序言）做 4 字节补丁；
2. Android 16/17 真机出现 BTI/MTE 相关的 SIGILL 或异常；
3. 需要「island 不足时明确失败」等更完整的失败语义。

引入 ShadowHook 的主要风险：arm64 `.so` 约 100KB（另有 hub 栈虚拟内存 3MB）、
`shadowhook_init` 初始化、与 LSPatch 自带 Dobby 的信号处理顺序、同一地址只能由一个框架
hook。

## 7. 验证证据

- Host：`hooks_registry` 240、`hooks_transaction` 585、`host_tests` 2781 全通过；
  `check_arch` 全绿（含 layers/resources/symbols/routes/log_policy）。
- 真机（monster v29，Android 12）：`0xf4d30` 为单条 `b`；`0xf4d34` 与 `0x1b5900`
  原指令完整；关闭/重开 world_tuning 能正确还原与重装；打怪不再卡死且掉落功能生效。

## 8. 未决项

1. 原始事故机（HUAWEI Android 16）尚未复测：需确认 4 字节补丁、近址槽与 16KB 页环境正常。
2. LSPatch 提供的 native API 版本号（用户提到的 “api 102”）未从二进制确认；不影响本决策，
   因为 `hook_func` 的补丁行为已实测。
