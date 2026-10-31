# 原版袋源（未接线、未实现）

本目录将承载原版袋（space 0 原版、space 1 改版/C18）的 `bags::BagSourceIo` 实现。

## 边界

- 只 include [`../bags/bag_source_io.h`](../bags/bag_source_io.h) 与 `contracts/`；不 include `orchestration/`。
- 原版袋内部移动不在本实现内做物品操作：`move_within` 委托 Host `move_item`，由原版代码完成。
- 读面与跨空间写面通过 Host 端口执行；不修改袋源编排层的注册表。
- 当前只有接口声明，没有 `.cpp`，未加入 `CMakeLists.txt`；不参与构建。

## 预计装配

未来由装配点创建两个实例（space 0、space 1），注入编排层的 `BagSourceRegistry`。
