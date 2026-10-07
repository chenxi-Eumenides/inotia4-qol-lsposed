#pragma once

#include "contracts/owner_identity.h"

namespace qol::host {

// identity 是宿主注册表的稳定实例地址，不是插件提交的名称或指针。
bool owner_register(void* identity);
bool owner_resume(void* identity);
void owner_suspend(void* identity);
bool owner_bind_store(void* identity, const char* store_namespace);
QolResult owner_verify_token(contracts::QolOwnerToken token);
QolResult owner_authorize_store(contracts::QolOwnerToken token, const char* store_namespace);
const contracts::QolOwnerIdentityApi& owner_identity_api();
// 清除测试装配，但不回退 token 分配序列。
void owner_identity_reset_for_test();

}  // namespace qol::host
