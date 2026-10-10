#pragma once

#include "contracts/owner_identity.h"

#include <string>

namespace qol::host {

// identity 是宿主注册表的稳定实例地址，不是插件提交的名称或指针。
bool owner_register(void* identity, const char* plugin_id = nullptr);
bool owner_resume(void* identity);
void owner_suspend(void* identity);
// Main-thread revocation: begin rejects new leases immediately; poll never waits for
// in-flight namespace calls. Cleanup notifications run outside the identity lock.
bool owner_suspend_begin(void* identity);
bool owner_suspend_poll(void* identity);
bool owner_resume_poll(void* identity);
// owner_suspend is the blocking compatibility wrapper for non-frame internal callers.
bool owner_bind_store(void* identity, const char* store_namespace);
bool owner_bind_slot_operation(void* identity);
QolResult owner_authorize_slot_operation(contracts::QolOwnerToken token);
bool owner_bind_save_route(void* identity);
QolResult owner_authorize_save_route(contracts::QolOwnerToken token);
QolResult owner_verify_token(contracts::QolOwnerToken token);
QolResult owner_plugin_id_for_token(contracts::QolOwnerToken token, std::string* out_plugin_id);
struct OwnerNamespaceCall {
    void* identity = nullptr;
    std::string plugin_id;
    bool active = false;
};
QolResult owner_namespace_call_begin(contracts::QolOwnerToken token, OwnerNamespaceCall* out_call);
void owner_namespace_call_end(OwnerNamespaceCall* call);
QolResult owner_authorize_store(contracts::QolOwnerToken token, const char* store_namespace);
const contracts::QolOwnerIdentityApi& owner_identity_api();
// 清除测试装配，但不回退 token 分配序列。
void owner_identity_reset_for_test();
void owner_store_lifecycle_handlers(void (*stopped)(uint64_t),
                                    void (*rebound)(uint64_t, uint64_t));
void owner_store_lifecycle_try_handlers(bool (*stopped)(uint64_t),
                                        bool (*rebound)(uint64_t, uint64_t));
void owner_save_route_stopped_handler(void (*stopped)(uint64_t));
void owner_save_route_stopped_try_handler(bool (*stopped)(uint64_t));

}  // namespace qol::host
