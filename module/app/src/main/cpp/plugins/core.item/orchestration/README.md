# 袋源编排层（未接线）

本目录将承载新的袋源编排，只依赖 [`../bags/bag_source_io.h`](../bags/bag_source_io.h)。

## 边界

- 负责：袋源注册表、按 space 查找、库存快照汇总、同空间/跨空间移动编排。
- 不负责：具体袋源的数据读写；不 include `native_source/` 或任何具体来源实现。
- 不直接调用游戏函数；具体来源实现负责 Host 端口调用。
- 当前未加入 `CMakeLists.txt`，不参与构建。

## 预计装配

未来由 `runtime/lifecycle.cpp` 或 `plugin.cpp` 创建原版袋源与注册袋源适配器，注入 `BagSourceRegistry`；编排层不感知来源类型。
