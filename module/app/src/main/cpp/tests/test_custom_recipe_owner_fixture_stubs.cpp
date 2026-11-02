// custom_recipe 单 owner 生产装配 fixture 的 Host SDK / Game Host 桩。
//
// 与 tests/test_stubs.cpp 同形（g_base、fn_resolve、ptr-slot 站点、game/read 依赖），
// 但 g_host_sdk 提供插件真正消费的函数面（能力查询、typed handler、事件、文本 scope、
// log、item handle），使真实 descriptor 能完成 install/registration 链路。
// 该文件只服务 custom_recipe_owner_fixture_tests；不与 test_stubs.cpp 同时链接。

#include <cstdint>
#include <cstring>

#include "contracts/commands.h"
#include "contracts/item_handle.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/item_handle.h"
#include "game/access/game_access.h"
#include "game/access/save.h"
#include "game/access/ui_choice.h"
#include "game/state/game_state.h"
#include "infra/capabilities.h"

uintptr_t g_base = 0;

// game/access/patches.cpp 的 BL 站点经 fn_resolve 解析目标；纯 fixture 恒等返回 vma。
uintptr_t fn_resolve(const char*, uintptr_t vma) { return vma; }

bool game_memory_accessible(const void*, size_t, char) { return true; }
uint32_t current_map_id() { return 0; }
void* lead_member() { return nullptr; }
CharGetAreaRectFn fn_char_get_area_rect = nullptr;

namespace qol::game {

void* g_ptr_slot_test_addr = nullptr;
uintptr_t g_ptr_slot_test_expected = 0;
bool g_ptr_slot_test_resolvable = false;

bool save_slot_delete_hook_target(void** out_slot, uintptr_t* out_expected) {
    if (!g_ptr_slot_test_resolvable) return false;
    if (out_slot != nullptr) *out_slot = g_ptr_slot_test_addr;
    if (out_expected != nullptr) *out_expected = g_ptr_slot_test_expected;
    return true;
}

bool ui_choice_button_slot_target(void** out_slot, uintptr_t* out_expected) {
    if (out_slot != nullptr) *out_slot = nullptr;
    if (out_expected != nullptr) *out_expected = 0;
    return false;
}

}  // namespace qol::game

namespace qol::host {
namespace {

// ---- 可观测的 Host SDK 面：fixture 用它断言「注册净额归零」----
QolResult sdk_capability_require(const char* id, QolVersionRange range, const void** out_iface) {
    if (out_iface == nullptr) return contracts::error(kQolErrInvalidArgument, nullptr, 0);
    const void* iface = infra::require_capability(id, range);
    *out_iface = iface;
    return iface != nullptr ? contracts::ok()
                            : contracts::error(kQolErrNotFound, "capability unavailable", 22);
}

int g_typed_registrations = 0;
int g_typed_unregistrations = 0;
QolResult sdk_register_typed_handler(uint32_t, contracts::QolTypedCommandHandler handler, void*) {
    if (handler == nullptr) return contracts::error(kQolErrInvalidArgument, nullptr, 0);
    ++g_typed_registrations;
    return contracts::ok();
}
QolResult sdk_unregister_typed_handler(uint32_t) {
    ++g_typed_unregistrations;
    return contracts::ok();
}
QolResult sdk_post_typed(uint32_t, const void*, uint32_t,
                         qol::contracts::QolCommandTicket* out_ticket) {
    if (out_ticket == nullptr) return contracts::error(kQolErrInvalidArgument, nullptr, 0);
    *out_ticket = 1;
    return contracts::ok();
}

int g_event_registrations = 0;
int g_event_unregistrations = 0;
QolResult sdk_event_subscribe(uint32_t, qol::contracts::QolEventHandler, void*, uint64_t* out_id) {
    ++g_event_registrations;
    if (out_id != nullptr) *out_id = 100u + static_cast<uint64_t>(g_event_registrations);
    return contracts::ok();
}
QolResult sdk_event_unsubscribe(uint64_t) {
    ++g_event_unregistrations;
    return contracts::ok();
}

int g_log_writes = 0;
QolResult sdk_log_write(int32_t, int32_t, const char*, const char*, uint32_t) {
    ++g_log_writes;
    return contracts::ok();
}

QolResult sdk_text_scope_push(int32_t) { return contracts::ok(); }
QolResult sdk_text_scope_pop() { return contracts::ok(); }

int g_item_handle_lookups = 0;
int32_t sdk_item_lookup(void*, qol::contracts::QolItemHandle* out, uint32_t* out_owner) {
    ++g_item_handle_lookups;
    if (out != nullptr) *out = contracts::kInvalidItemHandle;
    if (out_owner != nullptr) *out_owner = contracts::kQolItemHandleOwnerGame;
    return contracts::kQolItemHandleStatusNotFound;
}
int32_t sdk_item_register(void*, uint32_t, qol::contracts::QolItemHandle* out) {
    if (out != nullptr) *out = qol::contracts::QolItemHandle{1, 1};
    return contracts::kQolItemHandleStatusOk;
}

contracts::QolHostSdk make_host_sdk() {
    contracts::QolHostSdk sdk{};
    sdk.abi = {sizeof(sdk), contracts::kHostSdkMajor, contracts::kHostSdkMinor};
    sdk.log.write = &sdk_log_write;
    sdk.commands.register_typed_handler = &sdk_register_typed_handler;
    sdk.commands.unregister_typed_handler = &sdk_unregister_typed_handler;
    sdk.commands.post_typed = &sdk_post_typed;
    sdk.events.subscribe = &sdk_event_subscribe;
    sdk.events.unsubscribe = &sdk_event_unsubscribe;
    sdk.capabilities.require = &sdk_capability_require;
    sdk.text_scope.push = &sdk_text_scope_push;
    sdk.text_scope.pop = &sdk_text_scope_pop;
    sdk.ui = nullptr;
    sdk.item_handle.lookup_item = &sdk_item_lookup;
    sdk.item_handle.register_item = &sdk_item_register;
    return sdk;
}

}  // namespace

extern const contracts::QolHostSdk g_host_sdk = make_host_sdk();
extern const contracts::QolGameHost g_game_host{
    {sizeof(contracts::QolGameHost), contracts::kGameHostMajor, contracts::kGameHostMinor},
    {}, {}, {}, {}, {}, {}};

int fixture_typed_registrations() { return g_typed_registrations; }
int fixture_typed_unregistrations() { return g_typed_unregistrations; }
int fixture_event_registrations() { return g_event_registrations; }
int fixture_event_unregistrations() { return g_event_unregistrations; }

}  // namespace qol::host
