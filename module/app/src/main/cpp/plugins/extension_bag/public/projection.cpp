#include "plugins/extension_bag/api/bag_source.h"
#include "plugins/extension_bag/core/ui_state.h"
#include "game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/state_access.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/inventory_action.h"

#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/runtime/extension_bag_port.h"
#include "plugins/extension_bag/core/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/model/virtual_bag_state.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/core/geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/core/tabs.h"
#include "plugins/extension_bag/core/drag_session.h"
#include "plugins/extension_bag/transaction/transaction.h"
#include "plugins/extension_bag/core/host_adapter.h"
#include "plugins/extension_bag/ui/host_ui.h"
#include "plugins/extension_bag/plugin.h"
#include "plugins/extension_bag/model/store_sell_rules.h"
#include "plugins/extension_bag/runtime/bag_state_store.h"

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

void virtual_bag_refresh_item_area_with_gate() {
    ::qol::extension_bag::ReentryLock lock;
    virtual_bag_call_original_refresh_item_area_with_guard();
    const bool projection_installed =
        qol::extension_bag::ui_state().objects.module_view_installed && qol::extension_bag::ui_state().objects.projected_item_root != nullptr &&
        virtual_bag::valid_index(qol::extension_bag::ui_state().objects.module_view_index);
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
        ::qol::extension_bag::ReentryLock lock;
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
        const bool source_is_apply_material = extension_source &&
            (read_item_is_jewel(source_item) ||
             read_item_is_enchant_scroll(source_item));
        // H-15 装备槽事件适配（R-50）：目标必须是装备类（item_is_equip 判定；
        // 类别数据不可用时 item_is_equip 为假，按非装备 fail-closed 直通原版
        // backup，不得 Blocked），且落点是装备槽控件（slot index >= 16）。
        const bool target_is_equip_slot = target_item != nullptr &&
            item_is_equip(target_item) && target_slot_index >= 16;
        if (!extension_bag_is_equip_control_source(
                event, extension_source, source_is_apply_material, target_is_equip_slot)) {
            return VirtualBagEquipControlEventResult::kNotExtension;
        }

        const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[source_bag][source_slot];
        const ModuleUseState& use = qol::extension_bag::ui_state().objects.module_object_use[source_bag][source_slot];
        const bool descriptor_valid = descriptor.category > 0 && descriptor.count > 0 &&
            descriptor.payload_size > 0 && descriptor.payload_size <= virtual_bag::kMaxSerializedItem &&
            qol::extension_bag::ui_state().objects.module_objects[source_bag][source_slot] == source_item &&
            qol::extension_bag::ui_state().objects.module_object_categories[source_bag][source_slot] == descriptor.category &&
            qol::extension_bag::ui_state().objects.module_object_hashes[source_bag][source_slot] == virtual_bag::payload_hash(descriptor) &&
            qol::extension_bag::ui_state().objects.module_object_handles[source_bag][source_slot] != 0 && use.generation != 0 &&
            !use.active && !use.pending_release;
        const auto& session = qol::extension_bag::ui_state().ui.extension_drag_session;
        const bool session_valid = qol::extension_bag::ui_state().objects.module_view_installed &&
            qol::extension_bag::ui_state().objects.module_view_index == source_bag && qol::extension_bag::ui_state().objects.projected_item_root != nullptr &&
            valid_child_locked(qol::extension_bag::ui_state().objects.projected_item_root, source_slot) == source_control &&
            session.phase != virtual_bag::DragPhase::kIdle &&
            session.source_bag == source_bag && session.source_slot == source_slot &&
            virtual_bag::drag_session_source_identity_matches(
                session, source_bag, source_slot,
                qol::extension_bag::ui_state().objects.module_object_handles[source_bag][source_slot]) &&
            virtual_bag::drag_session_generation_current(session, qol::extension_bag::ui_state().ui.extension_tab_generation);
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
        ::qol::extension_bag::ReentryLock lock;
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
        ::qol::extension_bag::ReentryLock lock;
        if (released) released = module_use_finalize_release_locked(release_plan);
    }
    if (!released) {
        ::qol::extension_bag::ReentryLock lock;
        cancel_projected_drag_session_locked("equip-control-native-release-failed");
        return VirtualBagEquipControlEventResult::kBlocked;
    }

    bool session_committed = false;
    {
        ::qol::extension_bag::ReentryLock lock;
        auto& session = qol::extension_bag::ui_state().ui.extension_drag_session;
        if (session.phase == virtual_bag::DragPhase::kPressed) {
            virtual_bag::session_on_native_moving(&session, qol::extension_bag::ui_state().ui.extension_tab_generation);
        }
        if (session.source_bag == source_bag && session.source_slot == source_slot &&
            virtual_bag::drag_session_generation_current(session, qol::extension_bag::ui_state().ui.extension_tab_generation) &&
            session.phase == virtual_bag::DragPhase::kNativeMoving &&
            virtual_bag::session_resolve_target(
                &session, qol::extension_bag::ui_state().ui.extension_tab_generation,
                virtual_bag::DragTargetKind::kEquipmentSlot) != virtual_bag::DragTransition::kIllegal &&
            virtual_bag::session_begin_transaction(
                &session, qol::extension_bag::ui_state().ui.extension_tab_generation) != virtual_bag::DragTransition::kIllegal &&
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
namespace {

bool item_handle_matches(void* item, qol::contracts::QolItemHandle expected) {
    qol::contracts::QolItemHandle current{};
    return item != nullptr && extension_bag_item_handle_for_pointer(item, &current) &&
           current.slot == expected.slot && current.generation == expected.generation;
}

}  // namespace

bool apply_extension_material_to_slot_locked(
    ::qol::extension_bag::ReentryLock& lock, int src_bag, int src_slot,
    int dst_bag, int dst_slot, qol::contracts::QolItemHandle material_handle,
    qol::contracts::QolItemHandle target_handle) {
    if (!lock.owns_lock() || !virtual_bag::valid_index(src_bag) ||
        !virtual_bag::valid_index(dst_bag) || src_bag != dst_bag ||
        src_slot < 0 || src_slot >= virtual_bag::kSlotCount ||
        dst_slot < 0 || dst_slot >= virtual_bag::kSlotCount ||
        !qol::extension_bag::ui_state().objects.module_view_installed ||
        qol::extension_bag::ui_state().objects.module_view_index != src_bag ||
        qol::extension_bag::ui_state().objects.projected_item_root == nullptr) {
        VIRTBAG_LOG("apply reject reason=context src=%d/%d dst=%d/%d", src_bag, src_slot,
                    dst_bag, dst_slot);
        return false;
    }

    const int capacity = ::qol::extension_bag::access::bag().capacities[src_bag];
    void* projected_root = qol::extension_bag::ui_state().objects.projected_item_root;
    void* live_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
    if (src_slot >= capacity || dst_slot >= capacity || live_root != projected_root) {
        VIRTBAG_LOG("apply reject reason=root src=%d/%d dst=%d/%d", src_bag, src_slot,
                    dst_bag, dst_slot);
        return false;
    }

    void* material = nullptr;
    void* target_item = nullptr;
    const bool material_ok = extension_bag_private_item_for_handle(material_handle, &material);
    const bool target_ok = extension_bag_private_item_for_handle(target_handle, &target_item);
    const bool source_object_ok = qol::extension_bag::ui_state().objects.module_objects[src_bag][src_slot] == material;
    const bool source_handle_ok = item_handle_matches(material, material_handle);
    const bool target_object_ok = qol::extension_bag::ui_state().objects.module_objects[dst_bag][dst_slot] == target_item;
    const bool target_handle_ok = item_handle_matches(target_item, target_handle);
    if (!material_ok || material == nullptr || !target_ok || target_item == nullptr ||
        !source_object_ok || !source_handle_ok || !target_object_ok || !target_handle_ok) {
        VIRTBAG_LOG("apply reject reason=identity src=%d/%d dst=%d/%d", src_bag, src_slot,
                    dst_bag, dst_slot);
        return false;
    }

    void* source_control = valid_child_locked(projected_root, src_slot);
    void* target_control = valid_child_locked(projected_root, dst_slot);
    void* source_data = source_control != nullptr ? extension_bag_ui_data(source_control) : nullptr;
    void* target_data = target_control != nullptr ? extension_bag_ui_data(target_control) : nullptr;
    const bool target_is_equip = target_item != nullptr && item_is_equip(target_item);
    if (source_data == nullptr || target_data == nullptr ||
        *reinterpret_cast<void**>(source_data) != material ||
        *reinterpret_cast<void**>(target_data) != target_item || !target_is_equip) {
        VIRTBAG_LOG("apply reject reason=controls src=%d/%d dst=%d/%d", src_bag, src_slot,
                    dst_bag, dst_slot);
        return false;
    }

    const int source_category = ::qol::extension_bag::access::bag().items[src_bag][src_slot].category;
    const bool source_is_jewel = read_item_is_jewel(material);
    const bool source_is_scroll = read_item_is_enchant_scroll(material);
    if (!source_is_jewel && !source_is_scroll) {
        VIRTBAG_LOG("apply reject reason=material kind=%d jewel=%d scroll=%d",
                    source_category, source_is_jewel ? 1 : 0, source_is_scroll ? 1 : 0);
        return false;
    }
    const char* kind = source_is_jewel ? "jewel" : "scroll";

    const auto& host = ::qol::extension_bag::access::host();
    int32_t apply_allowed = 0;
    if (host.game == nullptr || host.character_extras == nullptr ||
        host.character_extras->read.can_apply_stuff == nullptr ||
        !qol::contracts::succeeded(host.character_extras->read.can_apply_stuff(
            target_item, material, &apply_allowed)) || apply_allowed == 0) {
        VIRTBAG_LOG("apply reject reason=not_applicable kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }

    const auto* save_facts = extension_bag_save_facts();
    const auto* write_api = extension_bag_write_api();
    if (save_facts == nullptr || save_facts->is_ok == nullptr || write_api == nullptr ||
        write_api->equipment.apply_stuff == nullptr) {
        VIRTBAG_LOG("apply reject reason=symbol_unresolved kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }

    ModuleUseToken use_token{};
    if (!module_use_begin_locked(src_bag, src_slot, material, &use_token)) {
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
        const bool aborted = module_use_abort_locked(use_token);
        const bool isolated = aborted || module_use_abort_isolate_locked(use_token);
        if (!isolated) {
            VIRTBAG_LOG_ERROR("ERROR apply token cleanup failed kind=%s equip=%p material=%p",
                              kind, target_item, material);
        }
        VIRTBAG_LOG("apply reject reason=is_ok_failed kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }

    bool consumed = false;
    ModuleUseReleasePlan release_plan{};
    const bool finished = module_use_finish_locked(use_token, &consumed, &release_plan);
    if (!finished || !consumed) {
        bool isolated = false;
        if (finished) {
            isolated = module_use_abort_isolate_locked(use_token);
        } else {
            const bool aborted = module_use_abort_locked(use_token);
            isolated = aborted || module_use_abort_isolate_locked(use_token);
        }
        if (!isolated) {
            VIRTBAG_LOG_ERROR("ERROR apply token cleanup failed kind=%s equip=%p material=%p",
                              kind, target_item, material);
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
            VIRTBAG_LOG("apply reject reason=material_release_failed kind=%s equip=%p material=%p",
                        kind, target_item, material);
            return false;
        }
    }

    void* post_apply_target = nullptr;
    const bool target_projection_current =
        qol::extension_bag::ui_state().objects.module_view_installed &&
        qol::extension_bag::ui_state().objects.module_view_index == dst_bag &&
        qol::extension_bag::ui_state().objects.projected_item_root == projected_root &&
        extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot) == projected_root &&
        extension_bag_private_item_for_handle(target_handle, &post_apply_target) &&
        post_apply_target == target_item &&
        qol::extension_bag::ui_state().objects.module_objects[dst_bag][dst_slot] == target_item &&
        item_handle_matches(target_item, target_handle);
    void* post_apply_control = target_projection_current
        ? valid_child_locked(projected_root, dst_slot) : nullptr;
    void* post_apply_data = post_apply_control != nullptr
        ? extension_bag_ui_data(post_apply_control) : nullptr;
    if (!target_projection_current || post_apply_data == nullptr ||
        *reinterpret_cast<void**>(post_apply_data) != target_item) {
        VIRTBAG_LOG("apply reject reason=stale_target kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }

    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(target_item, &payload, &payload_size)) {
        VIRTBAG_LOG("apply reject reason=target_payload_sync_failed kind=%s equip=%p material=%p",
                    kind, target_item, material);
        return false;
    }
    virtual_bag::Item& target_descriptor = ::qol::extension_bag::access::bag().items[dst_bag][dst_slot];
    target_descriptor.payload = payload;
    target_descriptor.payload_size = payload_size;
    // descriptor.count 恒为 canonical（R-38/R-48；getter 关闭态只返回 b 视图）。
    if (read_item_cumulate_count(target_item, &target_descriptor.count)) {
        target_descriptor.count = static_cast<int>(virtual_bag_canonical_count(target_item, 1));
    }
    qol::extension_bag::ui_state().objects.module_object_hashes[dst_bag][dst_slot] = virtual_bag::payload_hash(target_descriptor);
    extension_bag_mark_dirty_locked();
    persist_state_locked();
    refresh_projected_module_view_after_move_locked();
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
    ::qol::extension_bag::ReentryLock lock;
    if (src_control == nullptr) {
        return VirtualBagProjectionDropResult::kNotExtension;
    }
    void* src_data = extension_bag_ui_data(src_control);
    void* item = src_data != nullptr ? *reinterpret_cast<void**>(src_data) : nullptr;
    int src_bag = -1, src_slot = -1;
    if (item == nullptr || !module_slot_of_item_locked(item, &src_bag, &src_slot)) {
        return VirtualBagProjectionDropResult::kNotExtension;
    }
    if (!qol::extension_bag::ui_state().objects.module_view_installed || dst_control == nullptr ||
        ::qol::extension_bag::access::host().sdk == nullptr || ::qol::extension_bag::access::host().sdk->ui == nullptr ||
        qol::extension_bag::ui_state().objects.module_view_index != src_bag || qol::extension_bag::ui_state().objects.projected_item_root == nullptr) {
        VIRTBAG_LOG("drop rejected: extension source view mismatch src=%d/%d view=%d root=%p",
                    src_bag, src_slot, qol::extension_bag::ui_state().objects.module_view_index, qol::extension_bag::ui_state().objects.projected_item_root);
        return VirtualBagProjectionDropResult::kRejected;
    }
    void* live_root = extension_bag_ui_root(qol::contracts::QolUiRootKind::kEquipItemRoot);
    void* projected_source = valid_child_locked(qol::extension_bag::ui_state().objects.projected_item_root, src_slot);
    if (live_root == nullptr || live_root != qol::extension_bag::ui_state().objects.projected_item_root ||
        projected_source != src_control) {
        VIRTBAG_LOG("drop rejected: extension source control is not current projection "
                    "src=%d/%d source=%p expected=%p root=%p live_root=%p",
                    src_bag, src_slot, src_control, projected_source,
                    qol::extension_bag::ui_state().objects.projected_item_root, live_root);
        return VirtualBagProjectionDropResult::kRejected;
    }
    const int dst_capacity = ::qol::extension_bag::access::bag().capacities[qol::extension_bag::ui_state().objects.module_view_index];
    int dst_slot = -1;
    for (int slot = 0; slot < dst_capacity; ++slot) {
        if (valid_child_locked(qol::extension_bag::ui_state().objects.projected_item_root, slot) == dst_control) {
            dst_slot = slot;
            break;
        }
    }
    if (dst_slot < 0) dst_slot = extension_bag_ui_item_slot(dst_control);
    const bool destination_control_matches =
        dst_slot >= 0 && dst_slot < dst_capacity &&
        valid_child_locked(qol::extension_bag::ui_state().objects.projected_item_root, dst_slot) == dst_control;
    if (!virtual_bag::projection_drop_context_matches(
            qol::extension_bag::ui_state().objects.module_view_index, src_bag, true, true, destination_control_matches)) {
        VIRTBAG_LOG("drop rejected: extension source invalid projected target src=%d/%d dst=%d",
                    src_bag, src_slot, dst_slot);
        return VirtualBagProjectionDropResult::kRejected;
    }
    if (qol::extension_bag::ui_state().ui.extension_drag_session.phase != virtual_bag::DragPhase::kIdle) {
        if (qol::extension_bag::ui_state().ui.extension_drag_session.source_bag != src_bag ||
            qol::extension_bag::ui_state().ui.extension_drag_session.source_slot != src_slot ||
            qol::extension_bag::ui_state().ui.extension_drag_session.view_generation != qol::extension_bag::ui_state().ui.extension_tab_generation ||
            !virtual_bag::drag_session_source_identity_matches(
                qol::extension_bag::ui_state().ui.extension_drag_session, src_bag, src_slot,
                qol::extension_bag::ui_state().objects.module_object_handles[src_bag][src_slot])) {
            VIRTBAG_LOG("drop rejected: extension source session mismatch src=%d/%d phase=%d",
                        src_bag, src_slot, static_cast<int>(qol::extension_bag::ui_state().ui.extension_drag_session.phase));
            return VirtualBagProjectionDropResult::kRejected;
        }
        if (qol::extension_bag::ui_state().ui.extension_drag_session.phase == virtual_bag::DragPhase::kPressed) {
            advance_projected_drag_session_locked();
        }
        const virtual_bag::DragTargetKind target_kind = qol::features::extension_bag::core::classify_drag_target(
            virtual_bag::kExtensionLogicalBagFirst + src_bag, dst_slot);
        if (virtual_bag::session_resolve_target(&qol::extension_bag::ui_state().ui.extension_drag_session,
                                                qol::extension_bag::ui_state().ui.extension_tab_generation, target_kind) ==
                 virtual_bag::DragTransition::kIllegal ||
             virtual_bag::session_begin_transaction(&qol::extension_bag::ui_state().ui.extension_drag_session,
                                                    qol::extension_bag::ui_state().ui.extension_tab_generation) ==
                 virtual_bag::DragTransition::kIllegal ||
             virtual_bag::session_on_release(&qol::extension_bag::ui_state().ui.extension_drag_session,
                                             qol::extension_bag::ui_state().ui.extension_tab_generation) !=
                 virtual_bag::DragTransition::kAdvanced ||
             !virtual_bag::session_claim_transaction(&qol::extension_bag::ui_state().ui.extension_drag_session)) {
            VIRTBAG_LOG("drop rejected: extension source session transition failed src=%d/%d",
                        src_bag, src_slot);
            return VirtualBagProjectionDropResult::kRejected;
        }
    }
    const int dst_bag = qol::extension_bag::ui_state().objects.module_view_index;
    const bool source_is_apply_material =
        read_item_is_jewel(item) || read_item_is_enchant_scroll(item);
    void* dst_data = extension_bag_ui_data(dst_control);
    void* target_item = dst_data != nullptr ? *reinterpret_cast<void**>(dst_data) : nullptr;
    // S-03 apply 准入（R-50）：同袋 + 扩展宝石/强化卷轴源 + 目标为装备类 +
    // IsApplyStuff 判真。目标类别用 item_is_equip 判定（kNotEncoded=装备类；
    // 数据不可用返回 false，按非装备 fail-closed 回退 swap），不得按
    // 「不可堆叠」近似装备。非 apply 候选必须 fallthrough 到下方扩展事务，
    // 不得吞事件或 Blocked。
    int32_t apply_allowed = 0;
    const bool apply_stuff_allowed = target_item != nullptr && ::qol::extension_bag::access::host().game != nullptr &&
        ::qol::extension_bag::access::host().character_extras != nullptr &&
        ::qol::extension_bag::access::host().character_extras->read.can_apply_stuff != nullptr &&
        qol::contracts::succeeded(::qol::extension_bag::access::host().character_extras->read.can_apply_stuff(
            target_item, item, &apply_allowed)) && apply_allowed != 0;
    const bool apply_candidate = extension_bag_is_apply_candidate(
        src_bag == dst_bag, source_is_apply_material,
        target_item != nullptr && item_is_equip(target_item), apply_stuff_allowed);
    if (apply_candidate) {
        qol::contracts::QolItemHandle material_handle{};
        qol::contracts::QolItemHandle target_handle{};
        const bool applied =
            extension_bag_item_handle_for_pointer(item, &material_handle) &&
            extension_bag_item_handle_for_pointer(target_item, &target_handle) &&
            apply_extension_material_to_slot_locked(
                lock, src_bag, src_slot, dst_bag, dst_slot, material_handle, target_handle);
        if (qol::extension_bag::ui_state().ui.extension_drag_session.phase ==
            virtual_bag::DragPhase::kTransactionInFlight) {
            virtual_bag::session_finish_transaction(
                &qol::extension_bag::ui_state().ui.extension_drag_session, applied);
        }
        return applied ? VirtualBagProjectionDropResult::kHandled
                       : VirtualBagProjectionDropResult::kRejected;
    }

    const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[src_bag][src_slot];
    const int move_count = static_cast<int>(virtual_bag_effective_count(
        item, static_cast<uint32_t>(descriptor.count)));
    lock.unlock();
    const auto* action = ::qol::features::extension_bag::inventory_action_api();
    const auto outcome = action != nullptr && move_count > 0
                             ? action->move({qol::features::extension_bag::kExtensionBagSpaceId, src_bag, src_slot}, move_count,
                                            {qol::features::extension_bag::kExtensionBagSpaceId, dst_bag, dst_slot})
                             : qol::contracts::InventoryMoveOutcome{
                                   qol::contracts::InventoryActionStatus::kUnsupported,
                                   0, 0, {qol::features::extension_bag::kExtensionBagSpaceId, dst_bag, dst_slot}, {}};
    const bool routed = outcome.status == qol::contracts::InventoryActionStatus::kOk;
    lock.lock();
    if (qol::extension_bag::ui_state().ui.extension_drag_session.phase ==
        virtual_bag::DragPhase::kTransactionInFlight) {
        virtual_bag::session_finish_transaction(
            &qol::extension_bag::ui_state().ui.extension_drag_session, routed);
    }
    if (routed) {
        refresh_projected_module_view_after_move_locked();
        VIRTBAG_LOG("drop routed ext->ext dst_slot=%d", dst_slot);
        return VirtualBagProjectionDropResult::kHandled;
    }
    VIRTBAG_LOG("drop rejected: inventory.action move status=%d src=%d/%d dst=%d",
                static_cast<int>(outcome.status), src_bag, src_slot, dst_slot);
    return VirtualBagProjectionDropResult::kRejected;
}
bool virtual_bag_module_view_installed() {
    qol::extension_bag::BagSnapshot snapshot{};
    if (qol::extension_bag::bag_snapshot(&snapshot)) return snapshot.module_view_installed;
    ::qol::extension_bag::ReentryLock lock;
    return qol::extension_bag::ui_state().objects.module_view_installed;
}

bool virtual_bag_original_item_input_blocked() {
    ::qol::extension_bag::ReentryLock lock;
    const bool blocked = ::qol::extension_bag::access::enabled().load() &&
                         (::qol::extension_bag::access::bag().mode == virtual_bag::Mode::kModule ||
                          ::qol::extension_bag::access::bag().mode == virtual_bag::Mode::kExitingModule);
    if (blocked) {
        VIRTBAG_LOG("original item input blocked mode=%d selected=%d",
                    static_cast<int>(::qol::extension_bag::access::bag().mode), ::qol::extension_bag::access::bag().selected);
    }
    return blocked;
}

bool virtual_bag_allow_original_tab_drop(void* control, void* source_control,
                                         uint64_t caller_offset) {
    ::qol::extension_bag::ReentryLock lock;
    if (!::qol::extension_bag::access::enabled().load() || !qol::extension_bag::ui_state().objects.module_view_installed ||
        control == nullptr || source_control == nullptr ||
        ::qol::extension_bag::access::host().sdk == nullptr || ::qol::extension_bag::access::host().sdk->ui == nullptr) {
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
            item, static_cast<unsigned long long>(caller_offset), qol::extension_bag::ui_state().objects.module_view_index,
            qol::extension_bag::ui_state().ui.extension_drag_session.phase != virtual_bag::DragPhase::kIdle ? 1 : 0,
            source_bag, source_slot, target_slot);
        return false;
    }
    // 落点是扩展投影格时，原版 INVEN_MoveItem 只会按窗口原版袋处理，
    // 不可能把物品写进扩展 sidecar；放行反而会提前消费一次拖拽。
    // 原版/改版 -> 扩展的唯一提交点是 0x18 release 路由（inventory.action.move）。
    const int view_bag = qol::extension_bag::ui_state().objects.module_view_index;
    void* projected_root = qol::extension_bag::ui_state().objects.projected_item_root;
    if (projected_root != nullptr && virtual_bag::valid_index(view_bag)) {
        const int capacity = ::qol::extension_bag::access::bag().capacities[view_bag];
        for (int slot = 0; slot < capacity; ++slot) {
            if (valid_child_locked(projected_root, slot) == control) {
                VIRTBAG_LOG(
                    "original drop blocked for extension target control=%p item=%p target_slot=%d",
                    control, item, target_slot);
                return false;
            }
        }
    }
    // 原版/改版落点：交回原版处理，合并与普通移动都由游戏自己的 INVEN_MoveItem 完成。
    return true;
}
// G-14（方案 C）：原版物品操作后 RefreshItemArea 会以 INVEN 重刷控件，窗口袋投影需重写。
// 投影只占窗口袋；其他袋调用为 no-op。调用方（data_op_*）不持模块锁，此处安全获锁。
bool virtual_bag_sync_projected_slot(int display_bag, int slot) {
    ::qol::extension_bag::ReentryLock lock;
    if (!qol::extension_bag::ui_state().objects.module_view_installed || !virtual_bag::valid_index(qol::extension_bag::ui_state().objects.module_view_index) ||
        ::qol::extension_bag::access::host().sdk == nullptr || ::qol::extension_bag::access::host().sdk->ui == nullptr ||
        !ensure_module_view_root_current_locked()) {
        return false;
    }
    if (!virtual_bag::valid_original_transaction_bag(display_bag) ||
        display_bag != original_bag_locked()) {
        return false;
    }
    if (slot < 0 || slot >= ::qol::extension_bag::access::bag().capacities[qol::extension_bag::ui_state().objects.module_view_index]) {
        return false;
    }
    return sync_projected_slot_locked(qol::extension_bag::ui_state().objects.module_view_index, slot);
}

bool virtual_bag_sync_projected_item_control(void* control) {
    if (control == nullptr) return false;
    ::qol::extension_bag::ReentryLock lock;
    if (!qol::extension_bag::ui_state().objects.module_view_installed || !virtual_bag::valid_index(qol::extension_bag::ui_state().objects.module_view_index) ||
        ::qol::extension_bag::access::host().sdk == nullptr || ::qol::extension_bag::access::host().sdk->ui == nullptr ||
        !ensure_module_view_root_current_locked()) {
        return false;
    }
    const int capacity = ::qol::extension_bag::access::bag().capacities[qol::extension_bag::ui_state().objects.module_view_index];
    for (int slot = 0; slot < capacity; ++slot) {
        if (extension_bag_ui_child_at(qol::extension_bag::ui_state().objects.projected_item_root, slot) == control) {
            if (!extension_bag_ui_set_control_item(
                    control, qol::extension_bag::ui_state().objects.module_objects[qol::extension_bag::ui_state().objects.module_view_index][slot])) {
                QOL_LOG_ERROR(QolDomain::kExtensionBag,
                              "ERROR projected control sync failed bag=%d slot=%d control=%p",
                              qol::extension_bag::ui_state().objects.module_view_index, slot, control);
                return false;
            }
            return true;
        }
    }
    return false;
}

// 商店扩展视图会把窗口原版袋容量字临时放大到扩展容量；若玩家在此视图下
// 触发原版库存写入（买入→INVEN_SaveItem），FindSaveSlot 会按放大容量找
// 空槽，可能把物品写进超过真实容量的槽位（恢复后物品丢失/错乱）。该函数
// 由 SaveItem hook wrapper 在 backup 前调用：商店投影激活时先恢复投影
// （容量字还原），让原版按真实容量入库。
void virtual_bag_store_restore_for_original_write() {
    ::qol::extension_bag::ReentryLock lock;
    if (store_module_view_installed_locked()) {
        VIRTBAG_LOG("store save gate: restore store module view before original write");
        store_restore_module_view_locked();
    }
}
