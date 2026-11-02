#pragma once

#include <cstddef>
#include <cstdint>

#include "contracts/save_pre_save.h"

namespace qol::features::core_save::save_pre_save {

using qol::contracts::QolOwnerToken;
using qol::contracts::SavePreSaveObserverFn;

// 观察者上限：固定容量避免保存路径上分配。
constexpr std::size_t kMaxObservers = 8;

// owner 由调用方校验；这里只维护注册表。
QolResult register_observer(QolOwnerToken owner, SavePreSaveObserverFn fn, void* user,
                            uint64_t* out_id);
// owner 匹配才移除；未知 id 视为已注销（幂等成功）。
QolResult unregister_observer(QolOwnerToken owner, uint64_t id);
// 主线程逐个调用观察者；重入调用直接返回，避免嵌套通知。
void notify(int32_t slot);
// 清理指定 owner 的全部观察者（插件停用）。
void unregister_owner(QolOwnerToken owner);
void reset_for_test();

}  // namespace qol::features::core_save::save_pre_save
