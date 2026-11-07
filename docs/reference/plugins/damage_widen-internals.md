# `damage_widen` 内部实现

## 分层与目录

`plugins/damage_widen/plugin.cpp` 声明 feature、资源与生命周期；`game/adapter_ports/game_host_port.cpp` 实现只读属性端口。

## 装配

Host 在 `host/assembly.cpp` 注册插件 descriptor。插件声明四条私有、可选、fail-soft 的 entry hook，均可恢复且无 gate；`on_resource_ready` 缓存原函数 backup，供停用/降级路径透传。

## 回绕点审计

四个目标符号在 c18、c18-v514、c18-v520、monster、overhaul 五个变种的 VMA 一致，且均由 `libgame.so` dynsym 导出。运行时按符号名解析，VMA 为 resolver 回退值。

| 函数 | 原公式 | 被替换的风险 |
|---|---|---|
| `CHAR_GetNormalDamage` | `(attr5 + 可选 attr109 + 1000) × attr4 / 1000` | 千分比乘积在 int32 中回绕 |
| `CHAR_GetNormalMagicDamage` | `(attr9 + 1000) × attr8 / 1000` | 千分比乘积在 int32 中回绕 |
| `CHAR_GetCriticalDamage` | `damage + damage × (attr3 + max(1000-attr2,0)) / 1000` | 增伤乘积在 int32 中回绕；首 ABI 参数为未使用参数 |
| `CHAR_CalcMagicalDefense` | `damage × (1000-attr11) / 1000` | 减伤乘积在 int32 中回绕；首 ABI 参数未使用 |

## 关键机制与数据流

wrapper 按原函数的属性读取顺序和次数读取角色数据；普通魔攻保留原函数前两次被丢弃的 getter 调用。公式使用 int64 中间值，除法保持 C++ 向零截断，最后钳制到 `[0, INT32_MAX]`。

反汇编中的 `/1000` 与 `/100` 相关除法可表现为乘魔数、移位与修正序列；该乘魔数实现向零截断，与 C++ 有符号除法语义一致。插件仅替换上述四条公式，不覆盖技能倍率、DOT、物防与其他伤害路径。

## 属性读面依赖与失败恢复

`QolGameHost.character_read.attr` 尾追加于 Game Host v2.11，Host 经既有 `F_GET_ATTR_VMA` 登记（`fn_resolve` 按 dynsym 名优先、VMA 回退）缓存 `CHAR_GetAttr` 指针后直接调用。该 API 含 buff/装备修正，只读直调与原函数在被调线程上的自调用等价，不设主线程门禁（与 item 只读面同一纪律）。接口版本/字段不可用或单次属性读取失败时，wrapper 该次调用整体透传原函数（fail-soft 降级为原生 int32 数学），每条资源首次失败产生一条 warning。

## 生命周期与已知限制

`plugin_on_enabled`/`plugin_on_disabled` 控制计算 gate；停用后游戏回到原生 int32 算术。缺少对应 hook 时 fail-soft 保留其原生路径。游戏内数值和崩溃回归尚未真机验收。

## 测试与验证映射

验收卡见 [`verification.md`](../../refactor/verification.md) 的 VM-DW-01。静态构建不能替代真机 300 万伤害、钳制上限和低伤害回归证据。

公开契约见 [`damage_widen.md`](damage_widen.md)。
