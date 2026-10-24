#pragma once

#include "plugins/extension_bag/runtime/state.h"

// L4（api/public/ui）访问业务状态的唯一入口。
// 线程契约：仅游戏主线程可调用；HTTP 查询走 bag_query 快照，不经过这里。
// 说明：当前返回 live 引用，是 strangler 的访问收口步骤；后续可把只读
// 页面逐步替换为 bag_query 快照，写路径仍留在用例层。
namespace qol::extension_bag::access {

inline virtual_bag::State& bag() { return runtime_state().objects.bag; }
inline ExtensionBagObjectState& objects() { return runtime_state().objects; }
inline ExtensionBagUiState& ui() { return runtime_state().ui; }
inline ExtensionBagPersistenceState& persistence() { return runtime_state().persistence; }
inline ExtensionBagStoreState& store() { return runtime_state().store; }
inline ExtensionBagHostPorts& host() { return runtime_state().host; }
inline std::atomic<bool>& enabled() { return runtime_state().enabled; }

}  // namespace qol::extension_bag::access
