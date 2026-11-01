// 生产装备页 UI hook fixture（5 条入口 hook，owner=extension_bag，自 core.item 迁入）。
// 直接编译生产 TU（ui/equip_hooks.cpp）；Host SDK / 共享状态经 runtime_state()/ui_state()
// 注入，host_adapter、EB 刷新路径与控件事件处理由本文件提供测试替身。
// 覆盖：5 条 claim 声明、backup 交付（含 RefreshItemArea trampoline）、三态分流的
// wrapper 行为，以及 inventory_refresh provider 的 call_original/update_shortcut。
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "plugins/extension_bag/ui/equip_hooks.cpp"

namespace {

using qol::contracts::QolHostSdk;
using qol::features::extension_bag::equip_hooks::bind_backup;
using qol::features::extension_bag::equip_hooks::call_refresh_item_area_original;
using qol::features::extension_bag::equip_hooks::equip_hooks_claims;
using qol::features::extension_bag::kInventoryRefreshApi;

int g_pass = 0;
int g_fail = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            ++g_pass;                                                   \
        } else {                                                        \
            ++g_fail;                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

// ---- 替身状态 ----
int g_enabled = 1;
int g_backpack_result = 0;
int g_character_result = 0;
int g_unequip_result = 0;
int g_confirm_use_result = 0;
int g_module_item = 0;
int g_backpack_calls = 0;
int g_character_calls = 0;
int g_unequip_calls = 0;
int g_confirm_use_calls = 0;
int g_refresh_gate_calls = 0;
int g_control_event_calls = 0;
int g_inflight_enter = 0;
int g_inflight_leave = 0;
int g_shortcut_calls = 0;
int32_t g_shortcut_last = -1;

int g_button_backup_calls = 0;
void* g_button_backup_arg = nullptr;
int g_confirm_backup_calls = 0;
void* g_confirm_backup_arg = nullptr;
int g_refresh_backup_calls = 0;
int g_control_backup_calls = 0;
uint64_t g_control_backup_result = 77;
VirtualBagEquipControlEventResult g_control_result =
    VirtualBagEquipControlEventResult::kNotExtension;
uint64_t g_control_result_value = 55;

void button_backup(void* button) {
    ++g_button_backup_calls;
    g_button_backup_arg = button;
}
void confirm_backup(void* item) {
    ++g_confirm_backup_calls;
    g_confirm_backup_arg = item;
}
void refresh_backup() { ++g_refresh_backup_calls; }
uint64_t control_backup(void* control, uint64_t event, void* x2, void* param) {
    ++g_control_backup_calls;
    (void)control;
    (void)event;
    (void)x2;
    (void)param;
    return g_control_backup_result;
}
void shortcut_update(int32_t category) {
    ++g_shortcut_calls;
    g_shortcut_last = category;
}

QolResult inflight_enter(qol::contracts::InFlightHandle handle) {
    CHECK(handle.resource_id != nullptr);
    ++g_inflight_enter;
    return qol::contracts::ok();
}
QolResult inflight_leave(qol::contracts::InFlightHandle) {
    ++g_inflight_leave;
    return qol::contracts::ok();
}

QolHostSdk g_sdk{};

void reset_state() {
    g_enabled = 1;
    g_backpack_result = 0;
    g_character_result = 0;
    g_unequip_result = 0;
    g_confirm_use_result = 0;
    g_module_item = 0;
    g_backpack_calls = g_character_calls = g_unequip_calls = g_confirm_use_calls = 0;
    g_refresh_gate_calls = g_control_event_calls = 0;
    g_inflight_enter = g_inflight_leave = 0;
    g_shortcut_calls = 0;
    g_shortcut_last = -1;
    g_button_backup_calls = g_confirm_backup_calls = 0;
    g_refresh_backup_calls = g_control_backup_calls = 0;
    g_button_backup_arg = nullptr;
    g_confirm_backup_arg = nullptr;
    g_control_backup_result = 77;
    g_control_result = VirtualBagEquipControlEventResult::kNotExtension;
    g_control_result_value = 55;
    auto& ui = qol::extension_bag::ui_state().ui;
    ui.extension_desc_item = nullptr;
    ui.backup_button_equip_exe = nullptr;
    ui.backup_button_unequip_exe = nullptr;
    ui.backup_ok_confirm_use_item = nullptr;
    ui.backup_equip_control_event_proc = nullptr;
    ui.backup_refresh_item_area = nullptr;
    g_sdk = {};
    g_sdk.abi = {sizeof(QolHostSdk), qol::contracts::kHostSdkMajor,
                 qol::contracts::kHostSdkMinor};
    g_sdk.in_flight = {&inflight_enter, &inflight_leave};
    runtime_state().host = {};
    runtime_state().host.sdk = &g_sdk;
    extension_bag_bind_player_update_shortcut(nullptr);
}

void bind_all_backups() {
    CHECK(bind_backup("hook:UIEquip_ButtonEquipExe", reinterpret_cast<void*>(&button_backup)) == 0);
    CHECK(bind_backup("hook:UIEquip_ButtonUnequipExe",
                      reinterpret_cast<void*>(&button_backup)) == 0);
    CHECK(bind_backup("hook:UIEquip_OKConfirmUseItem",
                      reinterpret_cast<void*>(&confirm_backup)) == 0);
    CHECK(bind_backup("hook:UIEquip_RefreshItemArea",
                      reinterpret_cast<void*>(&refresh_backup)) == 0);
    CHECK(bind_backup("hook:UIEquip_EquipControlEventProc",
                      reinterpret_cast<void*>(&control_backup)) == 0);
}

void test_claims_declaration() {
    reset_state();
    uint32_t count = 0;
    const qol::contracts::ResourceClaim* claims = equip_hooks_claims(&count);
    CHECK(count == 5 && claims != nullptr);
    const char* ids[] = {"hook:UIEquip_ButtonEquipExe", "hook:UIEquip_ButtonUnequipExe",
                         "hook:UIEquip_OKConfirmUseItem", "hook:UIEquip_RefreshItemArea",
                         "hook:UIEquip_EquipControlEventProc"};
    const char* symbols[] = {"F_UIEQUIP_BUTTON_EQUIP_EXE_VMA",
                             "F_UIEQUIP_BUTTON_UNEQUIP_EXE_VMA",
                             "F_UIEQUIP_OK_CONFIRM_USE_ITEM_VMA",
                             "F_UIEQUIP_REFRESH_ITEM_AREA_VMA",
                             "F_UIEQUIP_EQUIP_CONTROL_EVENT_PROC_VMA"};
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(std::strcmp(claims[i].id, ids[i]) == 0);
        CHECK(std::strcmp(claims[i].symbol, symbols[i]) == 0);
        CHECK(claims[i].mechanism == qol::contracts::ResourceMechanism::kEntryHook);
        CHECK(claims[i].policy == qol::contracts::ResourcePolicy::kPrivate);
        CHECK(claims[i].requirement == qol::contracts::ResourceRequirement::kRequired);
        CHECK(claims[i].failure == qol::contracts::ResourceFailure::kFailSoft);
        CHECK(claims[i].resumable);
        CHECK(std::strcmp(claims[i].owner, "extension_bag") == 0);
        CHECK(claims[i].step_budget == 120);
        CHECK(claims[i].gate == nullptr);
        CHECK(claims[i].handler != nullptr);
    }
    CHECK(equip_hooks_claims(nullptr) == claims);
}

void test_backup_delivery() {
    reset_state();
    void* marker = reinterpret_cast<void*>(0x1234);
    bind_all_backups();
    auto& ui = qol::extension_bag::ui_state().ui;
    CHECK(ui.backup_button_equip_exe == reinterpret_cast<void*>(&button_backup));
    CHECK(ui.backup_button_unequip_exe == reinterpret_cast<void*>(&button_backup));
    CHECK(ui.backup_ok_confirm_use_item == reinterpret_cast<void*>(&confirm_backup));
    CHECK(ui.backup_refresh_item_area == reinterpret_cast<void*>(&refresh_backup));
    CHECK(ui.backup_equip_control_event_proc == reinterpret_cast<void*>(&control_backup));
    // 未知 id / 空 backup / 空 id 一律非 0（宿主 fail-closed 回滚）。
    CHECK(bind_backup("hook:UIEquip_OKDestroyItem", marker) == 1);
    CHECK(bind_backup("ptrslot:MOVE_MERGE_GOT", marker) == 1);
    CHECK(bind_backup("hook:UIEquip_ButtonEquipExe", nullptr) == 1);
    CHECK(bind_backup(nullptr, marker) == 1);
    CHECK(ui.backup_button_equip_exe == reinterpret_cast<void*>(&button_backup));
    // RefreshItemArea 的 backup 槽即 inventory_refresh trampoline。
    CHECK(call_refresh_item_area_original());
    CHECK(g_refresh_backup_calls == 1);
}

void test_equip_button_flow() {
    reset_state();
    bind_all_backups();
    int button = 0;
    // 关闭态：backup 透传，不进入扩展处理。
    g_enabled = 0;
    qol::features::extension_bag::equip_hooks::equip_button_exe_wrapper(&button);
    CHECK(g_button_backup_calls == 1 && g_button_backup_arg == &button);
    CHECK(g_backpack_calls == 0 && g_character_calls == 0);

    // 开启态：背包 equip 命中即返回。
    reset_state();
    bind_all_backups();
    g_backpack_result = 1;
    qol::features::extension_bag::equip_hooks::equip_button_exe_wrapper(&button);
    CHECK(g_backpack_calls == 1 && g_character_calls == 0 && g_button_backup_calls == 0);

    // 背包未命中 → 角色 equip 命中。
    reset_state();
    bind_all_backups();
    g_character_result = 1;
    qol::features::extension_bag::equip_hooks::equip_button_exe_wrapper(&button);
    CHECK(g_backpack_calls == 1 && g_character_calls == 1 && g_button_backup_calls == 0);

    // 详情为扩展物品：两个 handler 都未命中时拒绝原版。
    reset_state();
    bind_all_backups();
    qol::extension_bag::ui_state().ui.extension_desc_item = reinterpret_cast<void*>(0x99);
    g_module_item = 1;
    qol::features::extension_bag::equip_hooks::equip_button_exe_wrapper(&button);
    CHECK(g_button_backup_calls == 0 && g_backpack_calls == 1 && g_character_calls == 1);

    // 原版物品：backup 透传。
    reset_state();
    bind_all_backups();
    g_module_item = 0;
    qol::features::extension_bag::equip_hooks::equip_button_exe_wrapper(&button);
    CHECK(g_button_backup_calls == 1 && g_button_backup_arg == &button);

    // InFlightGuard 成对进出。
    CHECK(g_inflight_enter == g_inflight_leave);

    // backup 缺失：不崩溃、不调用。
    reset_state();
    g_module_item = 0;
    qol::features::extension_bag::equip_hooks::equip_button_exe_wrapper(&button);
    CHECK(g_button_backup_calls == 0);
}

void test_unequip_and_confirm_use() {
    reset_state();
    bind_all_backups();
    int button = 0;
    g_unequip_result = 1;
    qol::features::extension_bag::equip_hooks::unequip_button_exe_wrapper(&button);
    CHECK(g_unequip_calls == 1 && g_button_backup_calls == 0);

    reset_state();
    bind_all_backups();
    g_unequip_result = 0;
    qol::features::extension_bag::equip_hooks::unequip_button_exe_wrapper(&button);
    CHECK(g_unequip_calls == 1 && g_button_backup_calls == 1);

    reset_state();
    bind_all_backups();
    int item = 0;
    g_confirm_use_result = 1;
    qol::features::extension_bag::equip_hooks::ok_confirm_use_item_wrapper(&item);
    CHECK(g_confirm_use_calls == 1 && g_confirm_backup_calls == 0);

    reset_state();
    bind_all_backups();
    g_confirm_use_result = 0;
    qol::features::extension_bag::equip_hooks::ok_confirm_use_item_wrapper(&item);
    CHECK(g_confirm_use_calls == 1 && g_confirm_backup_calls == 1 &&
          g_confirm_backup_arg == &item);
}

void test_refresh_item_area() {
    reset_state();
    bind_all_backups();
    // 关闭态：直接透传 backup，不进入 EB 刷新路径。
    g_enabled = 0;
    qol::features::extension_bag::equip_hooks::refresh_item_area_wrapper();
    CHECK(g_refresh_backup_calls == 1 && g_refresh_gate_calls == 0);

    // 开启态：走 EB 自身 refresh gate（带投影 guard），不再直调 backup。
    reset_state();
    bind_all_backups();
    qol::features::extension_bag::equip_hooks::refresh_item_area_wrapper();
    CHECK(g_refresh_gate_calls == 1 && g_refresh_backup_calls == 0);

    // 无 backup：EB 刷新不可用，wrapper 记错并返回。
    reset_state();
    qol::features::extension_bag::equip_hooks::refresh_item_area_wrapper();
    CHECK(g_refresh_gate_calls == 0 && g_refresh_backup_calls == 0);
}

void test_equip_control_event_tristate() {
    reset_state();
    bind_all_backups();
    int control = 0;
    int param = 0;

    g_control_result = VirtualBagEquipControlEventResult::kHandled;
    g_control_result_value = 55;
    CHECK(qol::features::extension_bag::equip_hooks::equip_control_event_proc_wrapper(
              &control, 4, nullptr, &param) == 55);
    CHECK(g_control_event_calls == 1 && g_control_backup_calls == 0);

    g_control_result = VirtualBagEquipControlEventResult::kBlocked;
    CHECK(qol::features::extension_bag::equip_hooks::equip_control_event_proc_wrapper(
              &control, 4, nullptr, &param) == 0);
    CHECK(g_control_event_calls == 2 && g_control_backup_calls == 0);

    g_control_result = VirtualBagEquipControlEventResult::kNotExtension;
    CHECK(qol::features::extension_bag::equip_hooks::equip_control_event_proc_wrapper(
              &control, 4, nullptr, &param) == 77);
    CHECK(g_control_backup_calls == 1);

    // 关闭态直接透传。
    reset_state();
    bind_all_backups();
    g_enabled = 0;
    CHECK(qol::features::extension_bag::equip_hooks::equip_control_event_proc_wrapper(
              &control, 4, nullptr, &param) == 77);
    CHECK(g_control_event_calls == 0 && g_control_backup_calls == 1);
}

void test_inventory_refresh_provider() {
    reset_state();
    CHECK(kInventoryRefreshApi.call_original != nullptr);
    CHECK(kInventoryRefreshApi.update_shortcut != nullptr);
    CHECK(kInventoryRefreshApi.update_shortcut_category != nullptr);
    // 无 trampoline：返回 false（调用方按 no-op 降级）。
    CHECK(!kInventoryRefreshApi.call_original());
    bind_all_backups();
    CHECK(kInventoryRefreshApi.call_original() && g_refresh_backup_calls == 1);
    // 兼容无参入口为 no-op。
    kInventoryRefreshApi.update_shortcut();
    CHECK(g_shortcut_calls == 0);
    // 按类别刷新经宿主注入的原版函数。
    extension_bag_bind_player_update_shortcut(&shortcut_update);
    kInventoryRefreshApi.update_shortcut_category(5);
    CHECK(g_shortcut_calls == 1 && g_shortcut_last == 5);
    kInventoryRefreshApi.update_shortcut_category(-1);
    CHECK(g_shortcut_calls == 1);
    extension_bag_bind_player_update_shortcut(nullptr);
    kInventoryRefreshApi.update_shortcut_category(5);
    CHECK(g_shortcut_calls == 1);
}

}  // namespace

// ---- 生产 TU 依赖的 EB 外部符号替身（host_adapter / 刷新路径 / 控件事件）----

bool virtual_bag_enabled() { return g_enabled != 0; }

bool module_slot_of_item_locked(void*, int* out_bag, int* out_slot) {
    if (out_bag != nullptr) *out_bag = 0;
    if (out_slot != nullptr) *out_slot = 0;
    return g_module_item != 0;
}

bool extension_handle_backpack_equip() {
    ++g_backpack_calls;
    return g_backpack_result != 0;
}
bool extension_handle_character_equip() {
    ++g_character_calls;
    return g_character_result != 0;
}
bool extension_handle_original_bag_unequip() {
    ++g_unequip_calls;
    return g_unequip_result != 0;
}
bool extension_confirm_use_item(void*) {
    ++g_confirm_use_calls;
    return g_confirm_use_result != 0;
}

void virtual_bag_refresh_item_area_with_gate() { ++g_refresh_gate_calls; }

VirtualBagEquipControlEventResult virtual_bag_handle_equip_control_event(
    void*, uint64_t, void*, void*, VirtualBagEquipControlEventBackup backup,
    uint64_t* out_result) {
    ++g_control_event_calls;
    (void)backup;
    if (out_result != nullptr) *out_result = g_control_result_value;
    return g_control_result;
}

int main() {
    test_claims_declaration();
    test_backup_delivery();
    test_equip_button_flow();
    test_unequip_and_confirm_use();
    test_refresh_item_area();
    test_equip_control_event_tristate();
    test_inventory_refresh_provider();
    CHECK(g_fail == 0);
    std::printf("extension_bag_equip_hooks: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
