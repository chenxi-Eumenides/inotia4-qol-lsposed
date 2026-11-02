#pragma once

// 测试专用：infra 全模块聚合重置（仅 host 测试构建包含，见 tests/CMakeLists.txt）。
// 生产库不编译本文件；模块级隔离断言请直接用各自的 *_reset_for_test()。

namespace qol::infra {

void reset_for_test();

}  // namespace qol::infra
