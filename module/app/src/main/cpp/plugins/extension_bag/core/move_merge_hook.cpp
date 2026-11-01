#include "plugins/extension_bag/core/move_merge_hook.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "contracts/game_ports/move_merge_port.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/inventory_projection.h"
#include "contracts/item_facts.h"
#include "contracts/item_type.h"
#include "contracts/ui.h"
#include "plugins/extension_bag/assembly/projection_registration.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/state.h"

namespace qol::features::extension_bag::merge {
namespace {

constexpr const char* kReason = "Owns the UIEquip item-proc wrapper and native merge gate";

std::atomic<bool> g_feature_enabled{false};
std::atomic<bool> g_wrapper_ready{false};

// Host SDK / move-merge 端口 / item.facts 全部经 EB 共享状态；插件 install 时绑定，
// deactivate 时清空。不保留独立 attach 面板。
const contracts::QolHostSdk* host_sdk() { return runtime_state().host.sdk; }
const contracts::QolMoveMergePort* move_port() { return runtime_state().host.move_merge; }

const contracts::ResourceClaim kMergeClaims[] = {{
    kMergeResource,
    kMergeResource,
    contracts::ResourceMechanism::kPtrSlot,
    contracts::ResourcePolicy::kPrivate,
    contracts::ResourceRequirement::kOptional,
    contracts::ResourceFailure::kFailClosed,
    true,
    "extension_bag",
    kReason,
    nullptr,
    120,
    reinterpret_cast<void*>(&ui_equip_inven_item_proc_wrapper),
}};

// 合并门禁：feature 关闭或 ptrslot 未就绪时 wrapper 只透传原版。
bool gate_enabled() {
    return g_feature_enabled.load(std::memory_order_acquire) &&
           g_wrapper_ready.load(std::memory_order_acquire);
}

const contracts::InventoryProjectionCallbacks* projection_api() {
    return &projection_callbacks();
}

void refresh_original() {
    // EB refresh 路径：带抑制 guard 的原版物品区域刷新（core/projection.cpp）。
    virtual_bag_call_original_refresh_item_area_with_guard();
}

const contracts::ItemFactsApi* item_facts_api() {
    const auto* facts = runtime_state().host.item_facts;
    return contracts::item_facts_api_size_valid(facts) ? facts : nullptr;
}

bool control_slot(void* control, int32_t* out_slot) {
    const auto* sdk = host_sdk();
    return sdk != nullptr && sdk->ui != nullptr && sdk->ui->control_item_slot != nullptr &&
           contracts::succeeded(sdk->ui->control_item_slot(control, out_slot));
}

bool control_identity(void* control, contracts::QolItemHandle* out_item) {
    const auto* sdk = host_sdk();
    return control != nullptr && sdk != nullptr && sdk->ui != nullptr &&
           sdk->ui->control_item_identity != nullptr && out_item != nullptr &&
           contracts::succeeded(sdk->ui->control_item_identity(control, out_item));
}

bool native_item(contracts::QolItemHandle item) {
    const auto* port = move_port();
    return port != nullptr && port->validate_native_item != nullptr &&
           contracts::succeeded(port->validate_native_item(item));
}

bool extension_item(const contracts::InventoryProjectionCallbacks* projection,
                    contracts::QolItemHandle item) {
    if (projection == nullptr || projection->identify_extension_item == nullptr) return false;
    int32_t bag = -1;
    int32_t slot = -1;
    return contracts::succeeded(projection->identify_extension_item(item, &bag, &slot));
}

// 只有卷轴/宝石这类“作用到装备上”的物品才允许走强化/镶嵌；药水等一律走交换。
// item.facts.item_type 是权威分类（kJewel/kEnchantScroll 等价旧 static_text
// "gem"/"scroll" 判定），迁移后不再依赖 ITEM 端口读静态文本。
bool is_apply_material(const contracts::ItemFactsApi* facts, contracts::QolItemHandle item) {
    if (facts == nullptr || facts->item_type == nullptr) return false;
    contracts::QolItemType type = contracts::QolItemType::kUnknown;
    if (!contracts::succeeded(facts->item_type(item, &type))) return false;
    return type == contracts::QolItemType::kJewel ||
           type == contracts::QolItemType::kEnchantScroll;
}

bool item_facts(const contracts::ItemFactsApi* facts, contracts::QolItemHandle item,
                contracts::QolItemInventoryView* view,
                int32_t* count) {
    if (facts == nullptr || facts->view == nullptr || facts->cumulate_count == nullptr ||
        facts->category == nullptr ||
        view == nullptr || count == nullptr) return false;
    *view = {};
    return contracts::succeeded(facts->view(item, view)) &&
           view->count_supported != 0 &&
           contracts::succeeded(facts->cumulate_count(item, count));
}

bool item_count_encoding(const contracts::ItemFactsApi* facts, contracts::QolItemHandle item) {
    if (facts == nullptr || facts->count_encoding == nullptr) return false;
    contracts::QolItemCountEncoding encoding = contracts::QolItemCountEncoding::kUnknown;
    if (!contracts::succeeded(facts->count_encoding(item, &encoding)) ||
        encoding != contracts::QolItemCountEncoding::kEncoded) return false;
    return true;
}

uint64_t call_original(void* control, uint64_t event, void* x2, void* param) {
    const auto* sdk = host_sdk();
    uint64_t result = 0;
    if (sdk == nullptr || sdk->hooks.call_original_control_proc == nullptr ||
        !contracts::succeeded(sdk->hooks.call_original_control_proc(
            control, event, x2, param, &result))) return 0;
    return result;
}

void projection_observe_post(const contracts::InventoryProjectionCallbacks* api,
                             uint64_t token, void* control, uint64_t event, void* x2,
                             void* param, uint64_t result) {
    const auto* current = projection_api();
    if (current != nullptr && current->observe_item_proc_post != nullptr) {
        current->observe_item_proc_post(token, control, event, x2, param, result);
    }
    (void)api;
}

void report_item_proc_source(const contracts::InventoryProjectionCallbacks* api, uint64_t token,
                             void* control, uint64_t event, void* x2, void* param,
                             void* source_control) {
    const auto* current = projection_api();
    if (current != nullptr && current->observe_item_proc_source != nullptr) {
        current->observe_item_proc_source(token, control, event, x2, param, source_control);
    }
    (void)api;
}


}  // namespace

uint64_t ui_equip_inven_item_proc_wrapper(void* control, uint64_t event, void* x2, void* param) {
    const contracts::QolHostSdk* sdk = host_sdk();
    const contracts::InFlightGuard guard(sdk == nullptr ? nullptr : &sdk->in_flight,
                                         {kMergeResource});
    if (!g_feature_enabled.load(std::memory_order_acquire)) {
        return call_original(control, event, x2, param);
    }
    const auto* projection = projection_api();
    const uint64_t observation = projection != nullptr && projection->observe_item_proc_pre != nullptr
                                     ? projection->observe_item_proc_pre(control, event, x2, param)
                                     : 0;
    const auto finish = [observation, control, event, x2, param](uint64_t result) {
        projection_observe_post(nullptr, observation, control, event, x2, param, result);
        return result;
    };

    if (event == 0x02 && param != nullptr) {
        void* source = nullptr;
        if (sdk != nullptr && sdk->ui != nullptr &&
            sdk->ui->item_proc_source_control != nullptr &&
            contracts::succeeded(sdk->ui->item_proc_source_control(control, event, param, &source))) {
            report_item_proc_source(projection, observation, control, event, x2, param, source);
            const auto* current = projection_api();
            if (current != nullptr && current->projection_drop != nullptr) {
                const auto outcome = current->projection_drop(control, source);
                if (outcome == contracts::MoveMergeProjectionDropResult::kHandled) return finish(1);
                if (outcome == contracts::MoveMergeProjectionDropResult::kRejected) return finish(0);
            }
            contracts::QolItemHandle source_handle{};
            const auto* current_projection = projection_api();
            if (!control_identity(source, &source_handle)) return finish(0);
            if (extension_item(current_projection, source_handle) || !native_item(source_handle)) {
                return finish(0);
            }
        }
    }

    // event 0x04：取 source 并校验归属，扩展/非原生物品拦截；随后按
    // original_item_input_blocked 决定放行原版或拦截。item_proc_source_control 为
    // 纯内存查询（ui_kit_controls.inc），两段合并后只读一次，行为逐字等价。
    if (event == 0x04) {
        void* source = nullptr;
        const bool has_source = param != nullptr && sdk != nullptr && sdk->ui != nullptr &&
                                sdk->ui->item_proc_source_control != nullptr &&
                                contracts::succeeded(sdk->ui->item_proc_source_control(
                                    control, event, param, &source));
        contracts::QolItemHandle source_handle{};
        const auto* current = projection_api();
        const bool has_identity = has_source && control_identity(source, &source_handle);
        if (!has_source || !has_identity) return finish(0);
        if (extension_item(current, source_handle) || !native_item(source_handle)) {
            return finish(0);
        }
        report_item_proc_source(projection, observation, control, event, x2, param, source);
        if (current != nullptr && current->original_item_input_blocked != nullptr &&
            current->original_item_input_blocked()) {
            if (current->allow_original_tab_drop != nullptr &&
                current->allow_original_tab_drop(control, source, 0)) {
                return finish(call_original(control, event, x2, param));
            }
            return finish(0);
        }
    }

    const contracts::QolMoveMergePort* port = move_port();
    if (gate_enabled() && event == 0x04 && param != nullptr && port != nullptr &&
        port->move_item != nullptr && port->current_bag != nullptr) {
        void* source = nullptr;
        if (sdk != nullptr && sdk->ui != nullptr &&
            sdk->ui->item_proc_source_control != nullptr &&
            contracts::succeeded(sdk->ui->item_proc_source_control(control, event, param, &source))) {
            report_item_proc_source(projection, observation, control, event, x2, param, source);
            contracts::QolItemHandle source_handle{};
            contracts::QolItemHandle destination_handle{};
            bool extension_source = false;
            const auto* current_projection = projection_api();
            if (current_projection != nullptr &&
                current_projection->identify_extension_item != nullptr &&
                control_identity(source, &source_handle)) {
                extension_source = extension_item(current_projection, source_handle);
            }
            if (control_identity(source, &source_handle) && control_identity(control, &destination_handle)) {
                contracts::QolItemInventoryView source_view{};
                contracts::QolItemInventoryView destination_view{};
                int32_t source_count = 0;
                int32_t destination_count = 0;
                int32_t source_category = -1;
                int32_t destination_category = -1;
                const auto* facts = item_facts_api();
                const bool facts_ready = native_item(source_handle) &&
                                         native_item(destination_handle) &&
                                         item_count_encoding(facts, source_handle) &&
                                         item_count_encoding(facts, destination_handle) &&
                                         item_facts(facts, source_handle, &source_view, &source_count) &&
                                         item_facts(facts, destination_handle, &destination_view,
                                                    &destination_count) &&
                                         contracts::succeeded(facts->category(
                                             source_handle, &source_category)) &&
                                         contracts::succeeded(facts->category(
                                             destination_handle, &destination_category));
                int32_t target_bag = -1;
                int32_t destination_slot = -1;
                int32_t source_slot = -1;
                const bool location_ready = contracts::succeeded(
                                                port->current_bag(&target_bag)) &&
                                            control_slot(control, &destination_slot) &&
                                            control_slot(source, &source_slot) &&
                                            target_bag >= 0 && target_bag <= 17 &&
                                            destination_slot >= 0 && destination_slot < 16 &&
                                            source_slot >= 0 && source_slot < 16;
                const bool stackable = source_view.count_supported != 0 &&
                                       destination_view.count_supported != 0;
                // UIEquip_IsApplyStuff 对同类别可堆叠物品也返回 1，不能用作合并前提；
                // 同类别的堆叠合并只由类别与容量决定，且只有可堆叠物品能合并。
                if (facts_ready && location_ready && stackable &&
                    source_category == destination_category &&
                    source_count > 0 && destination_count > 0) {
                    int32_t limit = 0;
                    if (facts->count_limit != nullptr) {
                        uint32_t count_limit = 0;
                        if (contracts::succeeded(facts->count_limit(destination_handle,
                                                                       &count_limit))) {
                            limit = static_cast<int32_t>(count_limit);
                        }
                    }
                    if (limit >= static_cast<int32_t>(destination_count) &&
                        source_count <= limit - destination_count) {
                        int32_t moved = 0;
                        const auto result = port->move_item(source_handle, source_count,
                                                            target_bag, destination_slot,
                                                            &moved);
                        if (contracts::succeeded(result) && moved != 0) {
                            const auto* current = projection_api();
                            if (current != nullptr && current->module_view_installed != nullptr &&
                                current->module_view_installed() &&
                                current->sync_projected_slot != nullptr) {
                                (void)current->sync_projected_slot(target_bag, source_slot);
                                (void)current->sync_projected_slot(target_bag, destination_slot);
                            }
                            refresh_original();
                            void* root = nullptr;
                            const auto* ui = sdk != nullptr ? sdk->ui : nullptr;
                            if (ui != nullptr && ui->find_root != nullptr && ui->set_touch_cursor != nullptr &&
                                contracts::succeeded(ui->find_root(
                                    contracts::QolUiRootKind::kEquipItemRoot, &root)) && root != nullptr) {
                                (void)ui->set_touch_cursor(root, nullptr);
                            }
                            return finish(0);
                        }
                    }
                }
                contracts::QolItemInventoryView swap_destination_view{};
                bool destination_equipment = false;
                if (facts != nullptr && facts->view != nullptr &&
                    contracts::succeeded(facts->view(destination_handle,
                                                     &swap_destination_view))) {
                    destination_equipment = swap_destination_view.equip != 0;
                }
                // 卷轴/宝石落到装备上交给原版做强化/镶嵌；其他同类堆以外的情况
                // （不同物品、药水对装备、不可堆叠物品）一律交换两个槽的对象。
                const bool material_apply =
                    destination_equipment && is_apply_material(facts, source_handle);
                if (location_ready && !material_apply && !extension_source &&
                    !extension_item(projection_api(), destination_handle) &&
                    port->swap_slots != nullptr) {
                    constexpr int32_t kFirstVariantBag = contracts::kQolOriginalBagCount;
                    const int32_t space = target_bag >= kFirstVariantBag
                                              ? contracts::kQolBagSpaceVariant
                                              : contracts::kQolBagSpaceOriginal;
                    const int32_t bag = target_bag >= kFirstVariantBag
                                            ? target_bag - kFirstVariantBag
                                            : target_bag;
                    contracts::QolBagRef source_ref{space, bag};
                    contracts::QolBagRef destination_ref{space, bag};
                    if (contracts::succeeded(port->swap_slots(
                            &source_ref, source_slot, &destination_ref,
                            destination_slot))) {
                        const auto* current = projection_api();
                        if (current != nullptr && current->module_view_installed != nullptr &&
                            current->module_view_installed() &&
                            current->sync_projected_slot != nullptr) {
                            (void)current->sync_projected_slot(target_bag, source_slot);
                            (void)current->sync_projected_slot(target_bag, destination_slot);
                        }
                        refresh_original();
                        return finish(0);
                    }
                }
            }
            if (extension_source) return finish(0);
        }
    }

    const uint64_t result = call_original(control, event, x2, param);
    const auto* current = projection_api();
    if (current != nullptr && current->module_view_installed != nullptr &&
        current->module_view_installed() && current->sync_projected_bag != nullptr) {
        (void)current->sync_projected_bag();
    }
    return finish(result);
}

const contracts::ResourceClaim* merge_claims(uint32_t* out_count) {
    if (out_count != nullptr) {
        *out_count = static_cast<uint32_t>(sizeof(kMergeClaims) / sizeof(kMergeClaims[0]));
    }
    return kMergeClaims;
}

void detach() {
    // SDK/端口/item.facts 绑定由插件 deactivate 统一清空；此处只关本 feature 门禁。
    g_wrapper_ready.store(false, std::memory_order_release);
    g_feature_enabled.store(false, std::memory_order_release);
}

void set_enabled(bool enabled) { g_feature_enabled.store(enabled, std::memory_order_release); }
// test seam
bool enabled() { return g_feature_enabled.load(std::memory_order_acquire); }
// test seam
bool wrapper_ready() { return g_wrapper_ready.load(std::memory_order_acquire); }
void set_wrapper_ready(bool ready) { g_wrapper_ready.store(ready, std::memory_order_release); }

bool is_merge_resource(const char* resource_id) {
    return resource_id != nullptr && std::strcmp(resource_id, kMergeResource) == 0;
}

}  // namespace qol::features::extension_bag::merge
