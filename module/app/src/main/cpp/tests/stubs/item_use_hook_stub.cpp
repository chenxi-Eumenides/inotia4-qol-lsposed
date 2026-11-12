// item_action 在 use/discard 前后调用这两个钩子做失败归因；本文件供不编译
// inventory_hooks.cpp 的 host 测试目标提供空实现。
void inventory_native_hook_begin_item_use() {}

bool inventory_native_hook_end_item_use() { return false; }
