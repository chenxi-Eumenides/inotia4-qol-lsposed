#include "game_ui_virtbag.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_port.h"
#include "plugins/extension_bag/assembly/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/ui/store_sell_rules.h"
#include "plugins/extension_bag/extension_bag_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

bool virtual_bag_native_call_active() {
    return g_in_native_call;
}

void virtual_bag_call_original_refresh_item_area_with_guard() {
    NativeCallScope native_call;
    // D2c-2b-1：经 inventory_refresh capability 现取 raw-original trampoline；能力缺失
    // （provider 未安装/撤销中）按既有 no-op 降级——不直呼 fn_ui_equip_refresh_item_area，
    // 否则会经已被 hook 的入口回入 wrapper（原 trampoline 存在的唯一原因）。
    const void* refresh_iface = nullptr;
    if (runtime_state().host.sdk == nullptr || runtime_state().host.sdk->capabilities.require == nullptr) return;
    const QolResult refresh_result = runtime_state().host.sdk->capabilities.require(
        qol::contracts::kInventoryRefreshCapabilityId,
        QolVersionRange{static_cast<uint16_t>(qol::contracts::kInventoryRefreshVersionMajor),
                        static_cast<uint16_t>(qol::contracts::kInventoryRefreshVersionMinor)},
        &refresh_iface);
    const auto* refresh = refresh_result.ok == kQolOk
        ? static_cast<const qol::contracts::InventoryRefreshApi*>(refresh_iface) : nullptr;
    if (refresh != nullptr && refresh->call_original != nullptr) {
        refresh->call_original();
    }
}
void virtual_bag_refresh_item_area_with_gate() {
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    virtual_bag_call_original_refresh_item_area_with_guard();
    const bool projection_installed =
        runtime_state().objects.module_view_installed && runtime_state().objects.projected_item_root != nullptr &&
        virtual_bag::valid_index(runtime_state().objects.module_view_index);
    if (!g_refresh_restore_suppress && projection_installed) {
        refresh_projection_if_overwritten_locked();
    }
}

VirtualBagEquipControlEventResult virtual_bag_handle_equip_control_event(
    void* control, uint64_t event, void* x2, void* param,
    VirtualBagEquipControlEventBackup backup, uint64_t* out_result) {
    if (out_result != nullptr) *out_result = 0;
    if (event != 0x04 || param == nullptr) {
        return VirtualBagEquipControlEventResult::kNotExtension;
    }
    void* source_control = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(param) + 8);
    void* source_item = nullptr;
    void* target_item = nullptr;
    int source_bag = -1;
    int source_slot = -1;
    int target_slot_index = -1;
    ModuleUseToken use_token{};
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        ensure_state_loaded_locked();
        // 控件指针只作为当前投影索引；data 和 item 必须在同一锁上下文重读，
        // 防止控件在锁外被重绑后仍沿用旧身份。
        void* source_data = source_control != nullptr
            ? extension_bag_ui_data(source_control) : nullptr;
        void* target_data = control != nullptr ? extension_bag_ui_data(control) : nullptr;
        source_item = source_data != nullptr ? *reinterpret_cast<void**>(source_data) : nullptr;
        target_item = target_data != nullptr ? *reinterpret_cast<void**>(target_data) : nullptr;
        target_slot_index = control != nullptr ? extension_bag_ui_item_slot(control) : -1;
        const bool extension_source =
            source_item != nullptr && module_slot_of_item_locked(source_item, &source_bag, &source_slot);
        const int source_category = extension_source
            ? runtime_state().objects.bag.items[source_bag][source_slot].category : 0;
        const bool source_is_apply_material = extension_source &&
            (read_item_is_jewel(source_category) ||
             read_item_is_enchant_scroll(source_category));
        // H-15 装备槽事件适配（R-50）：目标必须是装备类（item_is_equip 判定；
        // 类别数据不可用时 item_is_equip 为假，按非装备 fail-closed 直通原版
        // backup，不得 Blocked），且落点是装备槽控件（slot index >= 16）。
        const bool target_is_equip_slot = target_item != nullptr &&
            item_is_equip(target_item) && target_slot_index >= 16;
        if (!extension_bag_is_equip_control_source(
                event, extension_source, source_is_apply_material, target_is_equip_slot)) {
            return VirtualBagEquipControlEventResult::kNotExtension;
        }

        const virtual_bag::Item& descriptor = runtime_state().objects.bag.items[source_bag][source_slot];
        const ModuleUseState& use = runtime_state().objects.module_object_use[source_bag][source_slot];
        const bool descriptor_valid = descriptor.category > 0 && descriptor.count > 0 &&
            descriptor.payload_size > 0 && descriptor.payload_size <= virtual_bag::kMaxSerializedItem &&
            runtime_state().objects.module_objects[source_bag][source_slot] == source_item &&
            runtime_state().objects.module_object_categories[source_bag][source_slot] == descriptor.category &&
            runtime_state().objects.module_object_hashes[source_bag][source_slot] == virtual_bag::payload_hash(descriptor) &&
            runtime_state().objects.module_object_handles[source_bag][source_slot] != 0 && use.generation != 0 &&
            !use.active && !use.pending_release;
        const auto& session = runtime_state().ui.extension_drag_session;
        const bool session_valid = runtime_state().objects.module_view_installed &&
            runtime_state().objects.module_view_index == source_bag && runtime_state().objects.projected_item_root != nullptr &&
            valid_child_locked(runtime_state().objects.projected_item_root, source_slot) == source_control &&
            session.phase != virtual_bag::DragPhase::kIdle &&
            session.source_bag == source_bag && session.source_slot == source_slot &&
            virtual_bag::drag_session_source_identity_matches(
                session, source_bag, source_slot,
                runtime_state().objects.module_object_handles[source_bag][source_slot]) &&
            virtual_bag::drag_session_generation_current(session, runtime_state().ui.extension_tab_generation);
        if (!descriptor_valid || !session_valid || backup == nullptr ||
            !module_use_begin_locked(source_bag, source_slot, source_item, &use_token)) {
            VIRTBAG_LOG("equip control blocked caller=UIEquip_EquipControlEventProc source=%p/%d/%d target=%p descriptor=%d session=%d",
                        source_item, source_bag, source_slot, target_item,
                        descriptor_valid ? 1 : 0, session_valid ? 1 : 0);
            cancel_projected_drag_session_locked("equip-control-validation-failed");
            return VirtualBagEquipControlEventResult::kBlocked;
        }
    }

    struct ActiveUseTokenScope {
        ModuleUseToken* previous = nullptr;
        explicit ActiveUseTokenScope(ModuleUseToken* current)
            : previous(g_active_module_use_token) {
            g_active_module_use_token = current;
        }
        ~ActiveUseTokenScope() { g_active_module_use_token = previous; }
    } active_use_token_scope(&use_token);
    const uint64_t original_result = backup(control, event, x2, param);

    bool consumed = false;
    bool finished = false;
    ModuleUseReleasePlan release_plan{};
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        finished = module_use_finish_locked(use_token, &consumed, &release_plan);
        if (extension_bag_finish_requires_abort(finished)) {
            const bool aborted = module_use_abort_locked(use_token);
            const bool isolated = aborted || module_use_abort_isolate_locked(use_token);
            VIRTBAG_LOG_ERROR("ERROR equip control token finish failed source=%p/%d/%d aborted=%d isolated=%d",
                        source_item, source_bag, source_slot, aborted ? 1 : 0,
                        isolated ? 1 : 0);
            cancel_projected_drag_session_locked("equip-control-finish-mismatch");
        } else if (!consumed) {
            VIRTBAG_LOG("equip control blocked caller=UIEquip_EquipControlEventProc source=%p/%d/%d target=%p finished=%d consumed=%d",
                        source_item, source_bag, source_slot, target_item,
                        finished ? 1 : 0, consumed ? 1 : 0);
            cancel_projected_drag_session_locked("equip-control-not-consumed");
        }
    }
    if (!finished || !consumed) return VirtualBagEquipControlEventResult::kBlocked;

    bool released = true;
    if (release_plan.pending) {
        released = module_use_release_native(release_plan);
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        if (released) released = module_use_finalize_release_locked(release_plan);
    }
    if (!released) {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        cancel_projected_drag_session_locked("equip-control-native-release-failed");
        return VirtualBagEquipControlEventResult::kBlocked;
    }

    bool session_committed = false;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        auto& session = runtime_state().ui.extension_drag_session;
        if (session.phase == virtual_bag::DragPhase::kPressed) {
            virtual_bag::session_on_native_moving(&session, runtime_state().ui.extension_tab_generation);
        }
        if (session.source_bag == source_bag && session.source_slot == source_slot &&
            virtual_bag::drag_session_generation_current(session, runtime_state().ui.extension_tab_generation) &&
            session.phase == virtual_bag::DragPhase::kNativeMoving &&
            virtual_bag::session_resolve_target(
                &session, runtime_state().ui.extension_tab_generation,
                virtual_bag::DragTargetKind::kEquipmentSlot) != virtual_bag::DragTransition::kIllegal &&
            virtual_bag::session_begin_transaction(
                &session, runtime_state().ui.extension_tab_generation) != virtual_bag::DragTransition::kIllegal &&
            virtual_bag::session_claim_transaction(&session) &&
            virtual_bag::session_finish_transaction(&session, true) ==
                virtual_bag::DragTransition::kAdvanced) {
            session_committed = true;
            VIRTBAG_LOG("equip control committed session token=%llu source=%d/%d target_slot=%d",
                        static_cast<unsigned long long>(session.token), source_bag, source_slot,
                        target_slot_index);
        } else {
            cancel_projected_drag_session_locked("equip-control-session-commit-failed");
        }
    }
    if (!session_committed) return VirtualBagEquipControlEventResult::kBlocked;
    VIRTBAG_LOG("equip control handled caller=UIEquip_EquipControlEventProc source=%p/%d/%d target=%p result=%llu",
                source_item, source_bag, source_slot, target_item,
                static_cast<unsigned long long>(original_result));
    if (out_result != nullptr) *out_result = original_result;
    return VirtualBagEquipControlEventResult::kHandled;
}
bool apply_extension_material_to_slot_locked(std::unique_lock<std::mutex>& lock,
                                             int src_bag, int src_slot, int dst_bag,
                                             int dst_slot, void* material, void* target_item) {
    const int source_category = runtime_state().objects.bag.items[src_bag][src_slot].category;
    const bool source_is_jewel = read_item_is_jewel(source_category);
    const char* kind = source_is_jewel ? "jewel" : "scroll";
    int32_t apply_allowed = 0;
    if (runtime_state().host.game == nullptr || runtime_state().host.character_extras->read.can_apply_stuff == nullptr ||
        !qol::contracts::succeeded(runtime_state().host.character_extras->read.can_apply_stuff(
            target_item, material, &apply_allowed)) || apply_allowed == 0) {
        if (runtime_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&runtime_state().ui.extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=not_applicable kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    const auto* save_facts = qol::features::extension_bag::save_facts();
    if (save_facts == nullptr || save_facts->is_ok == nullptr ||
        extension_bag_write_api() == nullptr ||
        extension_bag_write_api()->equipment.apply_stuff == nullptr) {
        if (runtime_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&runtime_state().ui.extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=symbol_unresolved kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    ModuleUseToken use_token{};
    if (!module_use_begin_locked(src_bag, src_slot, material, &use_token)) {
        if (runtime_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&runtime_state().ui.extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=stale_token kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    VIRTBAG_LOG("apply branch kind=%s equip=%p material=%p", kind, target_item, material);
    ModuleUseToken* previous_use_token = g_active_module_use_token;
    g_active_module_use_token = &use_token;
    lock.unlock();
    const bool save_ok = read_save_is_ok();
    const bool apply_ok = save_ok && extension_apply_stuff(target_item, material);
    g_active_module_use_token = previous_use_token;
    lock.lock();

    if (!apply_ok) {
        module_use_abort_locked(use_token);
        if (runtime_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&runtime_state().ui.extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=is_ok_failed kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    bool consumed = false;
    ModuleUseReleasePlan release_plan{};
    const bool finished = module_use_finish_locked(use_token, &consumed, &release_plan);
    if (!finished || !consumed) {
        if (finished) {
            module_use_abort_isolate_locked(use_token);
        } else if (!module_use_abort_locked(use_token)) {
            module_use_abort_isolate_locked(use_token);
        }
        if (runtime_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&runtime_state().ui.extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=%s kind=%s equip=%p material=%p",
                    finished ? "native_apply_failed" : "stale_token",
                    kind, target_item, material);
        return false;
    }
    if (release_plan.pending) {
        lock.unlock();
        const bool released = module_use_release_native(release_plan);
        lock.lock();
        if (!released || !module_use_finalize_release_locked(release_plan)) {
            if (runtime_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
                virtual_bag::session_finish_transaction(&runtime_state().ui.extension_drag_session, false);
            }
            VIRTBAG_LOG("apply reject reason=material_release_failed kind=%s equip=%p material=%p",
                        kind, target_item, material);
            return false;
        }
    }
    if (runtime_state().objects.module_objects[dst_bag][dst_slot] != target_item) {
        if (runtime_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&runtime_state().ui.extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=stale_target kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(target_item, &payload, &payload_size)) {
        if (runtime_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(&runtime_state().ui.extension_drag_session, false);
        }
        VIRTBAG_LOG("apply reject reason=target_payload_sync_failed kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    virtual_bag::Item& target_descriptor = runtime_state().objects.bag.items[dst_bag][dst_slot];
    target_descriptor.payload = payload;
    target_descriptor.payload_size = payload_size;
    // descriptor.count 恒为 canonical（R-38/R-48；getter 关闭态只返回 b 视图）。
    if (read_item_cumulate_count(target_item, &target_descriptor.count)) {
        target_descriptor.count = static_cast<int>(virtual_bag_canonical_count(target_item, 1));
    }
    runtime_state().objects.module_object_hashes[dst_bag][dst_slot] = virtual_bag::payload_hash(target_descriptor);
    extension_bag_mark_dirty_locked();
    persist_state_locked();
    refresh_projected_module_view_after_move_locked();
    if (runtime_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
        virtual_bag::session_finish_transaction(&runtime_state().ui.extension_drag_session, true);
    }
    VIRTBAG_LOG("apply committed kind=%s equip=%p material=%p", kind, target_item, material);
    return true;
}

// G-8：item proc 0x02（drop 到扩展格控件）路由——src=模块拖动状态（press 时
// projection_slot_at 记录），dst=落点控件索引。必须区分“非扩展源”“扩展事务成功”
// 和“扩展源但事务拒绝”：最后一种也必须吞掉原版事件，避免 INVEN_MoveItem 改写
// 真实物理槽。src/dst 必须属于当前 g_module_view_index 对应的 projected root；
// stale 控件或错视图一律是 kRejected。
VirtualBagProjectionDropResult virtual_bag_projection_drop_to_slot(void* dst_control,
                                                                     void* src_control) {
    std::unique_lock<std::mutex> lock(runtime_state().mtx);
    if (src_control == nullptr) {
        return VirtualBagProjectionDropResult::kNotExtension;
    }
    void* src_data = extension_bag_ui_data(src_control);
    void* item = src_data != nullptr ? *reinterpret_cast<void**>(src_data) : nullptr;
    int src_bag = -1, src_slot = -1;
    if (item == nullptr || !module_slot_of_item_locked(item, &src_bag, &src_slot)) {
        return VirtualBagProjectionDropResult::kNotExtension;
    }
    if (!runtime_state().objects.module_view_installed || dst_control == nullptr ||
        runtime_state().host.sdk == nullptr || runtime_state().host.sdk->ui == nullptr ||
        runtime_state().objects.module_view_index != src_bag || runtime_state().objects.projected_item_root == nullptr) {
        VIRTBAG_LOG("drop rejected: extension source view mismatch src=%d/%d view=%d root=%p",
                    src_bag, src_slot, runtime_state().objects.module_view_index, runtime_state().objects.projected_item_root);
        return VirtualBagProjectionDropResult::kRejected;
    }
    void* live_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
    void* projected_source = valid_child_locked(runtime_state().objects.projected_item_root, src_slot);
    if (live_root == nullptr || live_root != runtime_state().objects.projected_item_root ||
        projected_source != src_control) {
        VIRTBAG_LOG("drop rejected: extension source control is not current projection "
                    "src=%d/%d source=%p expected=%p root=%p live_root=%p",
                    src_bag, src_slot, src_control, projected_source,
                    runtime_state().objects.projected_item_root, live_root);
        return VirtualBagProjectionDropResult::kRejected;
    }
    const int dst_slot = extension_bag_ui_item_slot(dst_control);
    const bool destination_control_matches =
        dst_slot >= 0 && dst_slot < runtime_state().objects.bag.capacities[runtime_state().objects.module_view_index] &&
        valid_child_locked(runtime_state().objects.projected_item_root, dst_slot) == dst_control;
    if (!virtual_bag::projection_drop_context_matches(
            runtime_state().objects.module_view_index, src_bag, true, true, destination_control_matches)) {
        VIRTBAG_LOG("drop rejected: extension source invalid projected target src=%d/%d dst=%d",
                    src_bag, src_slot, dst_slot);
        return VirtualBagProjectionDropResult::kRejected;
    }
    if (runtime_state().ui.extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
        if (runtime_state().ui.extension_drag_session.source_bag != src_bag ||
            runtime_state().ui.extension_drag_session.source_slot != src_slot ||
            runtime_state().ui.extension_drag_session.view_generation != runtime_state().ui.extension_tab_generation ||
            !virtual_bag::drag_session_source_identity_matches(
                runtime_state().ui.extension_drag_session, src_bag, src_slot,
                runtime_state().objects.module_object_handles[src_bag][src_slot])) {
            VIRTBAG_LOG("drop rejected: extension source session mismatch src=%d/%d phase=%d",
                        src_bag, src_slot, static_cast<int>(runtime_state().ui.extension_drag_session.phase));
            return VirtualBagProjectionDropResult::kRejected;
        }
        if (runtime_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kPressed) {
            advance_projected_drag_session_locked();
        }
        const virtual_bag::DragTargetKind target_kind = virtual_bag::classify_drag_target(
            virtual_bag::kExtensionLogicalBagFirst + src_bag, dst_slot);
        if (virtual_bag::session_resolve_target(&runtime_state().ui.extension_drag_session,
                                                runtime_state().ui.extension_tab_generation, target_kind) ==
                 virtual_bag::DragTransition::kIllegal ||
             virtual_bag::session_begin_transaction(&runtime_state().ui.extension_drag_session,
                                                    runtime_state().ui.extension_tab_generation) ==
                 virtual_bag::DragTransition::kIllegal ||
             virtual_bag::session_on_release(&runtime_state().ui.extension_drag_session,
                                             runtime_state().ui.extension_tab_generation) !=
                 virtual_bag::DragTransition::kAdvanced ||
             !virtual_bag::session_claim_transaction(&runtime_state().ui.extension_drag_session)) {
            VIRTBAG_LOG("drop rejected: extension source session transition failed src=%d/%d",
                        src_bag, src_slot);
            return VirtualBagProjectionDropResult::kRejected;
        }
    }
    const int dst_bag = runtime_state().objects.module_view_index;
    const int source_category = runtime_state().objects.bag.items[src_bag][src_slot].category;
    const bool source_is_apply_material =
        read_item_is_jewel(source_category) || read_item_is_enchant_scroll(source_category);
    void* dst_data = extension_bag_ui_data(dst_control);
    void* target_item = dst_data != nullptr ? *reinterpret_cast<void**>(dst_data) : nullptr;
    // S-03 apply 准入（R-50）：同袋 + 扩展宝石/强化卷轴源 + 目标为装备类 +
    // IsApplyStuff 判真。目标类别用 item_is_equip 判定（kNotEncoded=装备类；
    // 数据不可用返回 false，按非装备 fail-closed 回退 swap），不得按
    // 「不可堆叠」近似装备。非 apply 候选必须 fallthrough 到下方扩展事务，
    // 不得吞事件或 Blocked。
    int32_t apply_allowed = 0;
    const bool apply_stuff_allowed = target_item != nullptr && runtime_state().host.game != nullptr &&
        runtime_state().host.character_extras->read.can_apply_stuff != nullptr &&
        qol::contracts::succeeded(runtime_state().host.character_extras->read.can_apply_stuff(
            target_item, item, &apply_allowed)) && apply_allowed != 0;
    const bool apply_candidate = extension_bag_is_apply_candidate(
        src_bag == dst_bag, source_is_apply_material,
        target_item != nullptr && item_is_equip(target_item), apply_stuff_allowed);
    if (apply_candidate) {
        return apply_extension_material_to_slot_locked(
            lock, src_bag, src_slot, dst_bag, dst_slot, item, target_item)
            ? VirtualBagProjectionDropResult::kHandled
            : VirtualBagProjectionDropResult::kRejected;
    }

    const bool routed = move_extension_to_extension_locked(src_bag, src_slot, src_bag, dst_slot);
    if (runtime_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kTransactionInFlight) {
        virtual_bag::session_finish_transaction(&runtime_state().ui.extension_drag_session, routed);
    }
    if (routed) {
        persist_state_locked();
        refresh_projected_module_view_after_move_locked();
        VIRTBAG_LOG("drop routed ext->ext dst_slot=%d", dst_slot);
        return VirtualBagProjectionDropResult::kHandled;
    }
    VIRTBAG_LOG("drop rejected: extension transaction refused src=%d/%d dst=%d",
                src_bag, src_slot, dst_slot);
    return VirtualBagProjectionDropResult::kRejected;
}
bool virtual_bag_module_view_installed() {
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    return runtime_state().objects.module_view_installed;
}

bool virtual_bag_original_item_input_blocked() {
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    const bool blocked = runtime_state().enabled.load() &&
                         (runtime_state().objects.bag.mode == virtual_bag::Mode::kModule ||
                          runtime_state().objects.bag.mode == virtual_bag::Mode::kExitingModule);
    if (blocked) {
        VIRTBAG_LOG("original item input blocked mode=%d selected=%d",
                    static_cast<int>(runtime_state().objects.bag.mode), runtime_state().objects.bag.selected);
    }
    return blocked;
}

bool virtual_bag_allow_original_tab_drop(void* control, void* source_control,
                                         uint64_t caller_offset) {
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (!runtime_state().enabled.load() || !runtime_state().objects.module_view_installed ||
        control == nullptr || source_control == nullptr ||
        runtime_state().host.sdk == nullptr || runtime_state().host.sdk->ui == nullptr) {
        return false;
    }
    if (extension_tab_index(control) >= 0) {
        return false;
    }
    const int target_slot = extension_bag_ui_item_slot(control);
    if (target_slot < 0) {
        return false;
    }
    void* source_data = extension_bag_ui_data(source_control);
    void* item = source_data != nullptr ? *reinterpret_cast<void**>(source_data) : nullptr;
    int source_bag = -1;
    int source_slot = -1;
    const bool extension_source = item != nullptr &&
                                   module_slot_of_item_locked(item, &source_bag, &source_slot);
    if (extension_source) {
        VIRTBAG_LOG(
            "drop rejected: extension source entering original proc item=%p caller=libgame+0x%llx "
            "view=%d session=%d source=%d/%d target_slot=%d",
            item, static_cast<unsigned long long>(caller_offset), runtime_state().objects.module_view_index,
            runtime_state().ui.extension_drag_session.phase != virtual_bag::DragPhase::kIdle ? 1 : 0,
            source_bag, source_slot, target_slot);
        return false;
    }
    const bool allowed = item != nullptr;
    if (allowed) {
        VIRTBAG_LOG("original drop allowed control=%p source=original item=%p target_slot=%d",
                    control, item, target_slot);
    }
    return allowed;
}
// G-14（方案 C）：原版物品操作后 RefreshItemArea 会以 INVEN 重刷控件，窗口袋投影需重写。
// 投影只占窗口袋；其他袋调用为 no-op。调用方（data_op_*）不持模块锁，此处安全获锁。
bool virtual_bag_sync_projected_slot(int display_bag, int slot) {
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (!runtime_state().objects.module_view_installed || !virtual_bag::valid_index(runtime_state().objects.module_view_index) ||
        runtime_state().host.sdk == nullptr || runtime_state().host.sdk->ui == nullptr ||
        !ensure_module_view_root_current_locked()) {
        return false;
    }
    if (!virtual_bag::valid_original_transaction_bag(display_bag) ||
        display_bag != original_bag_locked()) {
        return false;
    }
    if (slot < 0 || slot >= runtime_state().objects.bag.capacities[runtime_state().objects.module_view_index]) {
        return false;
    }
    return sync_projected_slot_locked(runtime_state().objects.module_view_index, slot);
}

bool virtual_bag_sync_projected_item_control(void* control) {
    if (control == nullptr) return false;
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (!runtime_state().objects.module_view_installed || !virtual_bag::valid_index(runtime_state().objects.module_view_index) ||
        runtime_state().host.sdk == nullptr || runtime_state().host.sdk->ui == nullptr ||
        !ensure_module_view_root_current_locked()) {
        return false;
    }
    const int capacity = runtime_state().objects.bag.capacities[runtime_state().objects.module_view_index];
    for (int slot = 0; slot < capacity; ++slot) {
        if (extension_bag_ui_child_at(runtime_state().objects.projected_item_root, slot) == control) {
            if (!extension_bag_ui_set_control_item(
                    control, runtime_state().objects.module_objects[runtime_state().objects.module_view_index][slot])) {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "ERROR projected control sync failed bag=%d slot=%d control=%p",
                              runtime_state().objects.module_view_index, slot, control);
                return false;
            }
            return true;
        }
    }
    return false;
}

bool virtual_bag_sync_projected_bag() {
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (!extension_bag_session_admitted_locked()) return false;
    refresh_projection_if_overwritten_locked();
    return runtime_state().objects.module_view_installed;
}
// 商店扩展视图会把窗口原版袋容量字临时放大到扩展容量；若玩家在此视图下
// 触发原版库存写入（买入→INVEN_SaveItem），FindSaveSlot 会按放大容量找
// 空槽，可能把物品写进超过真实容量的槽位（恢复后物品丢失/错乱）。该函数
// 由 SaveItem hook wrapper 在 backup 前调用：商店投影激活时先恢复投影
// （容量字还原），让原版按真实容量入库。
void virtual_bag_store_restore_for_original_write() {
    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (store_module_view_installed_locked()) {
        VIRTBAG_LOG("store save gate: restore store module view before original write");
        store_restore_module_view_locked();
    }
}
