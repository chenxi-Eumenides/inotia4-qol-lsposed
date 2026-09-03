# Native 源码规则

- `game_symbols.h` 是游戏版本常量唯一来源；禁止在域文件写裸 VMA、偏移或地址。
- 依赖方向固定为 bridge → feature/core、feature → core；core 不得依赖 feature。
- JNI 导出只保留参数转换和结果转换，业务逻辑放入对应 feature/core。
- 扩展背包持有 `g_virtual_bag_mtx` 时，不得调用会触发缓存刷新的 `op_ok()`。
- 拆分文件只移动职责，不在同一变更中修改业务语义。
- 每次修改后执行 host tests、Android debug 构建和对应真机验收。
