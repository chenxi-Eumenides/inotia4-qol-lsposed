#include "game_ui_virtbag.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/extension_bag_context.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/extension_bag_geometry.h"
#include "plugins/extension_bag/extension_bag_internal.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/assembly/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
#include "plugins/extension_bag/model/sale_transaction.h"
#include "plugins/extension_bag/model/draw_state_lock.h"
#include "plugins/extension_bag/model/main_menu_cleanup_edge.h"
#include "plugins/extension_bag/plugin.h"

#include <array>
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

namespace {

void extension_destroy_ok(void*);
void extension_sell_ok(void*);
void extension_destroy_cancel(void*);
void extension_sell_cancel(void*);

}

void restore_extension_desc_popup_callbacks_locked() {
    if (runtime_state().host.game != nullptr &&
        runtime_state().host.write_port->ui.restore_confirmation != nullptr) {
        (void)runtime_state().host.write_port->ui.restore_confirmation(
            1, reinterpret_cast<void*>(&extension_destroy_ok),
            reinterpret_cast<void*>(&extension_destroy_cancel),
            reinterpret_cast<void*>(runtime_state().ui.extension_destroy_original_ok),
            reinterpret_cast<void*>(runtime_state().ui.extension_destroy_original_cancel));
        (void)runtime_state().host.write_port->ui.restore_confirmation(
            0, reinterpret_cast<void*>(&extension_sell_ok),
            reinterpret_cast<void*>(&extension_sell_cancel),
            reinterpret_cast<void*>(runtime_state().ui.extension_sell_original_ok),
            reinterpret_cast<void*>(runtime_state().ui.extension_sell_original_cancel));
    }
    runtime_state().ui.extension_destroy_original_ok = nullptr;
    runtime_state().ui.extension_sell_original_ok = nullptr;
    runtime_state().ui.extension_destroy_original_cancel = nullptr;
    runtime_state().ui.extension_sell_original_cancel = nullptr;
    runtime_state().ui.extension_sell_apply_variant_discount = true;
}

namespace {

void extension_sell_cancel(void*) {
    ExtensionDestroyOkFn original_cancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        original_cancel = runtime_state().ui.extension_sell_original_cancel;
        restore_extension_desc_popup_callbacks_locked();
        reset_extension_desc_item_hooks_locked();
        runtime_state().ui.extension_desc_item = nullptr;
        runtime_state().ui.extension_desc_item_bag = -1;
        runtime_state().ui.extension_desc_item_slot = -1;
    }
    extension_bag_ui_description_set_off();
    if (original_cancel != nullptr) original_cancel(nullptr);
}

void extension_destroy_cancel(void*) {
    ExtensionDestroyOkFn original_cancel = nullptr;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        original_cancel = runtime_state().ui.extension_destroy_original_cancel;
        restore_extension_desc_popup_callbacks_locked();
        reset_extension_desc_item_hooks_locked();
        runtime_state().ui.extension_desc_item = nullptr;
        runtime_state().ui.extension_desc_item_bag = -1;
        runtime_state().ui.extension_desc_item_slot = -1;
    }
    if (original_cancel != nullptr) original_cancel(nullptr);
}

void extension_sell_ok(void*) {
    VIRTBAG_LOG("extension sell confirmation entered");
    void* item = nullptr;
    int bag = -1;
    int slot = -1;
    int count = 0;
    int64_t price = 0;
    bool apply_variant_discount = true;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        restore_extension_desc_popup_callbacks_locked();
        item = runtime_state().ui.extension_desc_item;
        bag = runtime_state().ui.extension_desc_item_bag;
        slot = runtime_state().ui.extension_desc_item_slot;
        apply_variant_discount = runtime_state().ui.extension_sell_apply_variant_discount;
        const bool current_item = item != nullptr && virtual_bag::valid_index(bag) &&
                                  slot >= 0 && slot < virtual_bag::kSlotCount &&
                                  runtime_state().objects.module_objects[bag][slot] == item &&
                                  module_object_release_allowed_locked(
                                      bag, slot, item, "extension sell", false, nullptr);
        if (!current_item) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension sell rejected stale detail bag=%d slot=%d item=%p", bag,
                        slot, item);
            reset_extension_desc_item_hooks_locked();
            runtime_state().ui.extension_desc_item = nullptr;
            runtime_state().ui.extension_desc_item_bag = -1;
            runtime_state().ui.extension_desc_item_slot = -1;
            clear_original_desc_locked();
            return;
        }
        if (runtime_state().host.game == nullptr || runtime_state().host.item_facts == nullptr || runtime_state().host.item_facts->sell_price == nullptr ||
            runtime_state().host.item_facts->is_no_sell == nullptr || extension_bag_write_api() == nullptr ||
            extension_bag_write_api()->equipment.add_money == nullptr ||
            extension_bag_write_api()->equipment.minus_money == nullptr) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension sell rejected symbols unavailable bag=%d slot=%d", bag, slot);
            clear_original_desc_locked();
            return;
        }
                // 卖出数量取模式视图（R-47 决策 b）：启用态 canonical、关闭态 b。
                count = static_cast<int>(virtual_bag_effective_count(
                    item, static_cast<uint32_t>(runtime_state().objects.bag.items[bag][slot].count)));
        int category = -1;
        int32_t no_sell = 0;
        if (!read_item_category(item, &category) ||
            !qol::contracts::succeeded(runtime_state().host.item_facts->is_no_sell(category, &no_sell)) ||
            no_sell != 0) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension sell rejected no-sell item bag=%d slot=%d category=%d",
                        bag, slot, category);
            clear_original_desc_locked();
            return;
        }
        int64_t unit_price = 0;
        if (!extension_bag_ui_read_item_sell_price(item, &unit_price)) {
            clear_original_desc_locked();
            return;
        }
        price = extension_bag_sell_price(item, count, apply_variant_discount);
        if (price < 0) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                "sell price reject reason=price_bounds source=equip category=%d count=%d unit=%lld variant=%d payload=%u gen=%llu",
                category, count, static_cast<long long>(unit_price),
                apply_variant_discount ? 70 : 100,
                virtual_bag::payload_hash(runtime_state().objects.bag.items[bag][slot]),
                static_cast<unsigned long long>(runtime_state().objects.module_object_use[bag][slot].generation));
            clear_original_desc_locked();
            return;
        }
        QOL_LOG_INFO(QolDomain::kExtensionBag,
            "sell price source=equip category=%d count=%d unit=%lld variant=%d final=%lld payload=%u gen=%llu",
            category,
            count,
            static_cast<long long>(unit_price), apply_variant_discount ? 70 : 100,
            static_cast<long long>(price),
            virtual_bag::payload_hash(runtime_state().objects.bag.items[bag][slot]),
            static_cast<unsigned long long>(runtime_state().objects.module_object_use[bag][slot].generation));
    }

    if (price < 0 || !extension_bag_ui_add_money(price)) {
        VIRTBAG_LOG("extension sell money credit failed bag=%d slot=%d price=%lld", bag, slot,
                    static_cast<long long>(price));
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        clear_original_desc_locked();
        return;
    }
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        const bool current_item = runtime_state().ui.extension_desc_item == item &&
                                  runtime_state().ui.extension_desc_item_bag == bag &&
                                  runtime_state().ui.extension_desc_item_slot == slot &&
                                  runtime_state().objects.module_objects[bag][slot] == item &&
                                  virtual_bag_effective_count(item, static_cast<uint32_t>(runtime_state().objects.bag.items[bag][slot].count)) ==
                                      static_cast<uint32_t>(count) &&
                                  module_object_release_allowed_locked(
                                      bag, slot, item, "extension sell commit", false, nullptr);
        if (current_item) {
            const virtual_bag::Item previous_item = runtime_state().objects.bag.items[bag][slot];
            runtime_state().objects.bag.items[bag][slot] = {};
            extension_bag_mark_dirty_locked();
            if (!free_module_object_locked(bag, slot)) {
                runtime_state().objects.bag.items[bag][slot] = previous_item;
                persist_state_locked();
                QOL_LOG_WARN(QolDomain::kExtensionBag, "extension sell rejected object release bag=%d slot=%d", bag, slot);
            } else {
                persist_state_locked();
                if (runtime_state().objects.module_view_installed && runtime_state().objects.module_view_index == bag) {
                    refresh_module_item_area_locked(bag);
                }
                removed = true;
            }
        }
    }
    if (!removed) {
        const bool refunded = extension_bag_ui_minus_money(price);
        VIRTBAG_LOG("extension sell remove failed bag=%d slot=%d price=%lld refunded=%d", bag,
                    slot, static_cast<long long>(price), refunded ? 1 : 0);
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        clear_original_desc_locked();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        reset_extension_desc_item_hooks_locked();
        runtime_state().ui.extension_desc_item = nullptr;
        runtime_state().ui.extension_desc_item_bag = -1;
        runtime_state().ui.extension_desc_item_slot = -1;
        clear_original_desc_locked();
    }
    VIRTBAG_LOG("extension sell committed bag=%d slot=%d price=%lld", bag, slot,
                static_cast<long long>(price));
}

void extension_destroy_ok(void*) {
    int committed_bag = -1;
    int committed_slot = -1;
    bool committed = false;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        restore_extension_desc_popup_callbacks_locked();

        const int bag = runtime_state().ui.extension_desc_item_bag;
        const int slot = runtime_state().ui.extension_desc_item_slot;
        if (runtime_state().ui.extension_desc_item != nullptr && virtual_bag::valid_index(bag) &&
            slot >= 0 && slot < virtual_bag::kSlotCount &&
            runtime_state().objects.module_objects[bag][slot] == runtime_state().ui.extension_desc_item &&
            module_object_release_allowed_locked(
                bag, slot, runtime_state().ui.extension_desc_item, "extension destroy", false, nullptr)) {
            const virtual_bag::Item previous_item = runtime_state().objects.bag.items[bag][slot];
            runtime_state().objects.bag.items[bag][slot] = {};
            extension_bag_mark_dirty_locked();
            if (!free_module_object_locked(bag, slot)) {
                runtime_state().objects.bag.items[bag][slot] = previous_item;
                persist_state_locked();
                QOL_LOG_WARN(QolDomain::kExtensionBag, "extension destroy rejected object release bag=%d slot=%d", bag, slot);
            } else {
                persist_state_locked();
                committed_bag = bag;
                committed_slot = slot;
                committed = true;
            }
            reset_extension_desc_item_hooks_locked();
            runtime_state().ui.extension_desc_item = nullptr;
            runtime_state().ui.extension_desc_item_bag = -1;
            runtime_state().ui.extension_desc_item_slot = -1;
            clear_original_desc_locked();
        } else {
            reset_extension_desc_item_hooks_locked();
            runtime_state().ui.extension_desc_item = nullptr;
            runtime_state().ui.extension_desc_item_bag = -1;
            runtime_state().ui.extension_desc_item_slot = -1;
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension destroy rejected stale detail bag=%d slot=%d", bag, slot);
        }
    }
    if (committed && runtime_state().host.game != nullptr &&
        runtime_state().host.write_port->ui.refresh_item_area != nullptr) {
        virtual_bag_call_original_refresh_item_area_with_guard();
    }
    if (committed) {
        VIRTBAG_LOG("extension destroy committed bag=%d slot=%d", committed_bag, committed_slot);
    }
}

}  // namespace

bool consume_extension_item_after_native_locked(int bag, int slot, void* item,
                                                int before_count, int observed_before,
                                                const char* context, void* raw_use_token) {
    if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
        item == nullptr || before_count <= 0) {
        return false;
    }
    int observed_count = before_count - 1;
    (void)extension_bag_ui_read_item_cumulate_count(item, &observed_count);
    int count = observed_count;
    if (count == observed_before) --count;
    if (count >= before_count) count = before_count - 1;
    if (count < 0) count = 0;
    uint32_t old_count_flags = 0;
    if (!extension_bag_ui_read_item_encoded_field(item, &old_count_flags)) return false;
    if (count == 0) {
        const virtual_bag::Item previous_item = runtime_state().objects.bag.items[bag][slot];
        runtime_state().objects.bag.items[bag][slot] = {};
        if (!free_module_object_locked(bag, slot, raw_use_token)) {
            runtime_state().objects.bag.items[bag][slot] = previous_item;
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension consume rejected object release context=%s bag=%d slot=%d",
                        context, bag, slot);
            return false;
        }
        return true;
    }
    const ::qol::contracts::QolItemCountEncoding count_encoding = extension_bag_ui_item_count_encoding(item);
    if (count_encoding != ::qol::contracts::QolItemCountEncoding::kEncoded) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension consume rejected count encoding=%d context=%s bag=%d slot=%d",
                    static_cast<int>(count_encoding), context, bag, slot);
        return false;
    }
    // S2 写回（R-47 决策 b 模式感知）：count 为 getter 返回的模式视图数量
    //（启用态 canonical 128a+b、关闭态 b），扣减后的写回经 item-count facade：
    // 启用态拆段写回借位自动完成（a 借位 → b+128）；关闭态只写 b、保留 a——
    // descriptor 随后回写 canonical 解码值，重开后读回完整值。非可堆叠类别已在
    // 上方按三态类别门控拒绝，不会触碰 marker/S2 重叠位段。
    if (!virtual_bag_write_effective(item, static_cast<uint32_t>(count))) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension consume rejected count write context=%s bag=%d slot=%d",
                     context, bag, slot);
        return false;
    }
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        const auto* count_api = item_count_api();
        if (count_api != nullptr && count_api->write_encoded_field != nullptr) {
            (void)count_api->write_encoded_field(item, old_count_flags);
        }
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension consume reserialize failed context=%s bag=%d slot=%d",
                    context, bag, slot);
        return false;
    }
    virtual_bag::Item& descriptor = runtime_state().objects.bag.items[bag][slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    // descriptor.count 恒为 canonical：关闭态 b 写保留 a，重开读回完整值。
    descriptor.count = static_cast<int>(virtual_bag_canonical_count(
        item, virtual_bag_canonical_from_field(old_count_flags,
                                               static_cast<uint32_t>(descriptor.count))));
    runtime_state().objects.module_object_categories[bag][slot] = descriptor.category;
    runtime_state().objects.module_object_hashes[bag][slot] = virtual_bag::payload_hash(descriptor);
    // ApplyStuff consumes a stacked material through H-04. Keep the token's
    // consumed bit even when the native object remains live, otherwise the
    // caller treats a successful jewel/scroll apply as an unconsumed failure.
    if (raw_use_token != nullptr) {
        ModuleUseToken* token = static_cast<ModuleUseToken*>(raw_use_token);
        ModuleUseState& use = runtime_state().objects.module_object_use[bag][slot];
        if (use.active && use.item == item && use.generation == token->generation) {
            use.consumed = true;
        }
    }
    return true;
}

// IsUseAfterConfirm 类扩展物品（如卓越灵药）的确认使用回调分流：
// 原版回调会先 INVEN_FindItemSlot（扩展物品不在 INVEN 被拦），此处直接
// CHAR_UseItemEx 后清理 hooks/描述。身份不一致时返回 false，由 Native Hook
// wrapper 调用原版 backup。
bool extension_confirm_use_item(void* item) {
    bool used = false;
    int bag = -1;
    int slot = -1;
    ModuleUseReleasePlan release_plan{};
    {
        std::unique_lock<std::mutex> lock(runtime_state().mtx, std::try_to_lock);
        if (!lock.owns_lock()) {
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension confirm-use lock busy item=%p", item);
            return false;
        }
        if (!extension_bag_enabled() || item == nullptr || item != runtime_state().ui.extension_desc_item) {
            return false;
        }
        bag = runtime_state().ui.extension_desc_item_bag;
        slot = runtime_state().ui.extension_desc_item_slot;
        int module_bag = -1;
        int module_slot = -1;
        if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
            runtime_state().objects.bag.items[bag][slot].category <= 0 ||
            runtime_state().objects.bag.items[bag][slot].count <= 0 ||
            runtime_state().objects.module_objects[bag][slot] != item ||
            !module_slot_of_item_locked(item, &module_bag, &module_slot) ||
            module_bag != bag || module_slot != slot) {
            return false;
        }
        void* character = extension_menu_character();
        if (character == nullptr || extension_bag_write_api() == nullptr ||
            extension_bag_write_api()->equipment.use_item == nullptr) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                        "extension confirm-use unavailable bag=%d slot=%d character=%p",
                        bag, slot, character);
            return false;
        }
        ModuleUseToken use_token{};
        if (!module_use_begin_locked(bag, slot, item, &use_token)) {
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension confirm-use already active bag=%d slot=%d item=%p",
                        bag, slot, item);
            return false;
        }
        // CHAR_UseItemEx 会同步进入 INVEN_ConsumeItem；不得持有背包锁调用。
        ModuleUseToken* previous_use_token = g_active_module_use_token;
        g_active_module_use_token = &use_token;
        lock.unlock();
        int use_result = 0;
        used = extension_bag_ui_use_item(character, item, 0, &use_result) && use_result != 0;
        lock.lock();
        g_active_module_use_token = previous_use_token;
        bool consumed = false;
        const bool use_token_valid = module_use_finish_locked(use_token, &consumed, &release_plan);
        const bool same_item = runtime_state().objects.module_objects[bag][slot] == item;
        if (!use_token_valid) {
            const bool marker_cleared = module_use_abort_locked(use_token);
            const bool isolated = marker_cleared || module_use_abort_isolate_locked(use_token);
            QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR extension confirm-use token lost after use bag=%d slot=%d item=%p marker_cleared=%d; treated handled",
                        bag, slot, item, isolated ? 1 : 0);
            if (isolated) {
                reset_extension_desc_item_hooks_locked();
                runtime_state().ui.extension_desc_item = nullptr;
                runtime_state().ui.extension_desc_item_bag = -1;
                runtime_state().ui.extension_desc_item_slot = -1;
            } else {
                QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR extension confirm-use abort failed; slot may remain active "
                            "bag=%d slot=%d item=%p",
                            bag, slot, item);
            }
            return true;
        }
        if (!used) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension confirm-use executed failed bag=%d slot=%d", bag, slot);
            return true;
        }
        if (!same_item && !consumed) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension confirm-use identity changed unexpectedly bag=%d slot=%d item=%p",
                        bag, slot, item);
            return true;
        }
    }
    if (release_plan.pending) {
        if (!module_use_release_native(release_plan)) {
            VIRTBAG_LOG_ERROR("ERROR extension confirm-use native release failed bag=%d slot=%d",
                        bag, slot);
            return true;
        }
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        if (!module_use_finalize_release_locked(release_plan)) {
            QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR extension confirm-use release finalize failed bag=%d slot=%d",
                        bag, slot);
            return true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        // native 释放和对象摘除完成后再刷新并清理详情控件，避免悬挂绑定。
        refresh_module_item_area_locked(bag);
        reset_extension_desc_item_hooks_locked();
        runtime_state().ui.extension_desc_item = nullptr;
        runtime_state().ui.extension_desc_item_bag = -1;
        runtime_state().ui.extension_desc_item_slot = -1;
    }
    VIRTBAG_LOG("extension confirm-use handled bag=%d slot=%d used=1", bag, slot);
    return true;
}

// R-63 佣兵徽章（category 42..50 / 928..933）使用按钮函数级接管。
// 原版 UIEquip_ButtonUseMercenarySealExe(0xb8144) 经 ControlItem_GetItem 从 g_inven
// 取物品，扩展物品不进物理袋会取到 null/错位而静默失败。此处按原版流程用模块详情
// 身份直接调原版 MERCENARYSYSTEM_MakeMercenary（0x119658），消耗交给已 Hook 的
// INVEN_RemoveItem（H-04）承接扩展槽删除；SAVE_IsOK/空槽校验顺序与原版一致。
// 返回 true=已接管（原版按钮不再执行）；false=非扩展详情佣兵徽章，交原版透传。
bool extension_use_mercenary_seal() {
    bool used = false;
    int bag = -1;
    int slot = -1;
    void* item = nullptr;
    ModuleUseReleasePlan release_plan{};
    {
        std::unique_lock<std::mutex> lock(runtime_state().mtx, std::try_to_lock);
        if (!lock.owns_lock()) {
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension mercenary-seal lock busy");
            return false;
        }
        const auto* save_facts = qol::features::extension_bag::save_facts();
        if (!extension_bag_enabled() || runtime_state().ui.extension_desc_item == nullptr ||
            runtime_state().host.game == nullptr || runtime_state().host.game->read_only.item.is_mercenary_seal == nullptr ||
            save_facts == nullptr || save_facts->is_ok == nullptr ||
            runtime_state().host.character_extras->read.has_empty_mercenary_slot == nullptr ||
            extension_bag_write_api() == nullptr ||
            extension_bag_write_api()->equipment.make_mercenary == nullptr) {
            return false;
        }
        item = runtime_state().ui.extension_desc_item;
        bag = runtime_state().ui.extension_desc_item_bag;
        slot = runtime_state().ui.extension_desc_item_slot;
        if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
            runtime_state().objects.module_objects[bag][slot] != item) {
            return false;
        }
        const virtual_bag::Item& descriptor = runtime_state().objects.bag.items[bag][slot];
        if (descriptor.category <= 0 || descriptor.count <= 0) return false;
        // 用物品自身 flags 的类别（与原版 SetDescMenu 创建 0x98 按钮同源）判定，
        // descriptor.category 仅作回退，避免序列化差异导致误判。
        int category = descriptor.category;
        (void)read_item_category(item, &category);
        if (!read_item_is_mercenary_seal(category)) return false;
    }
    // 原版按钮顺序：UIDesc_SetOff → SAVE_IsOK → IsEmptyManagerSlot → MakeMercenary。
    // 游戏函数一律在 g_virtual_bag_mtx 外调用（MakeMercenary 内部会进入
    // INVEN_RemoveItem → H-04 再次取锁，持锁调用必死锁）。
    extension_bag_ui_description_set_off();
    const auto show_popup = [](uint32_t text_id) {
        if (runtime_state().host.sdk != nullptr && runtime_state().host.sdk->ui != nullptr &&
            runtime_state().host.sdk->ui->show_text_popup != nullptr) {
            (void)runtime_state().host.sdk->ui->show_text_popup(static_cast<int32_t>(text_id));
        }
    };
    if (!extension_bag_ui_read_save_is_ok()) {
        VIRTBAG_LOG("extension mercenary-seal reject reason=save_busy bag=%d slot=%d", bag, slot);
        show_popup(0x25);
        return true;
    }
    if (!extension_bag_ui_read_has_empty_mercenary_slot()) {
        VIRTBAG_LOG("extension mercenary-seal reject reason=slots_full bag=%d slot=%d", bag, slot);
        show_popup(0x17);
        return true;
    }
    ModuleUseToken use_token{};
    bool begin_failed = false;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        if (runtime_state().objects.module_objects[bag][slot] != item ||
            !module_use_begin_locked(bag, slot, item, &use_token)) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension mercenary-seal begin failed bag=%d slot=%d item=%p", bag, slot,
                        item);
            begin_failed = true;
        }
    }
    // UI 弹窗会进入原版 UI，不在背包锁内创建。
    if (begin_failed) {
        show_popup(0xc);
        return true;
    }
    ModuleUseToken* previous_use_token = g_active_module_use_token;
    g_active_module_use_token = &use_token;
    void* mercenary = nullptr;
    (void)extension_bag_ui_make_mercenary(item, &mercenary);
    g_active_module_use_token = previous_use_token;
    used = mercenary != nullptr;
    bool token_lost = false;
    bool make_failed = false;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        bool consumed = false;
        if (!module_use_finish_locked(use_token, &consumed, &release_plan)) {
            const bool marker_cleared = module_use_abort_locked(use_token);
            const bool isolated = marker_cleared || module_use_abort_isolate_locked(use_token);
            QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR extension mercenary-seal token lost bag=%d slot=%d item=%p "
                          "marker_cleared=%d; treated handled",
                          bag, slot, item, isolated ? 1 : 0);
            if (isolated) {
                reset_extension_desc_item_hooks_locked();
                runtime_state().ui.extension_desc_item = nullptr;
                runtime_state().ui.extension_desc_item_bag = -1;
                runtime_state().ui.extension_desc_item_slot = -1;
            }
            token_lost = true;
        } else if (!used) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension mercenary-seal make failed bag=%d slot=%d", bag, slot);
            make_failed = true;
        }
    }
    if (token_lost) return true;
    if (make_failed) {
        show_popup(0xc);
        return true;
    }
    if (release_plan.pending) {
        if (!module_use_release_native(release_plan)) {
            VIRTBAG_LOG_ERROR("ERROR extension mercenary-seal native release failed bag=%d slot=%d", bag,
                        slot);
            return true;
        }
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        if (!module_use_finalize_release_locked(release_plan)) {
            QOL_LOG_ERROR(QolDomain::kExtensionBag, "ERROR extension mercenary-seal release finalize failed bag=%d slot=%d", bag,
                        slot);
            return true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        // native 释放和对象摘除完成后再刷新并清理详情控件，避免悬挂绑定。
        refresh_module_item_area_locked(bag);
        reset_extension_desc_item_hooks_locked();
        runtime_state().ui.extension_desc_item = nullptr;
        runtime_state().ui.extension_desc_item_bag = -1;
        runtime_state().ui.extension_desc_item_slot = -1;
    }
    show_popup(0x18);
    VIRTBAG_LOG("extension mercenary-seal handled bag=%d slot=%d used=1", bag, slot);
    return true;
}

void extension_desc_item_execute_at(size_t index, void* button) {
    if (index >= runtime_state().ui.extension_desc_item_bound.size() ||
        !runtime_state().ui.extension_desc_item_bound[index] || button == nullptr) return;
    const uint64_t id = runtime_state().ui.extension_desc_item_ids[index];
    const auto* api = qol::contracts::desc_hook_v2_if_available(runtime_state().host.sdk);
    if (api == nullptr || id == 0 || button == nullptr) return;
    qol::contracts::QolDescButtonTarget current{};
    int32_t matches = 0;
    const QolResult validate_result = api->validate(id, button, &current, &matches);
    if (!qol::contracts::succeeded(validate_result) || matches == 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "extension desc hook stale index=%d id=%llu button=%p current_button=%p data=%p execute_slot=%p code=%d matches=%d reason=identity_validation_failed",
                     static_cast<int>(index), static_cast<unsigned long long>(id), button,
                     current.button, current.data, current.execute_slot,
                     validate_result.code, matches);
        return;
    }
    const int hook_index = static_cast<int>(index);
    void* original = current.original_proc;
    void* item = nullptr;
    int item_bag = -1;
    int item_slot = -1;
    {
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        item = runtime_state().ui.extension_desc_item;
        item_bag = runtime_state().ui.extension_desc_item_bag;
        item_slot = runtime_state().ui.extension_desc_item_slot;
    }
    VIRTBAG_LOG("extension desc execute index=%d original=%p bag=%d slot=%d",
                hook_index, original, item_bag, item_slot);

    const bool direct_use = extension_desc_original_matches(
        id, "desc.button.use");
    const bool confirm_use = extension_desc_original_matches(
        id, "desc.button.use_after_confirm");
    if (direct_use || confirm_use) {
        bool used = false;
        bool current_item = false;
        void* leader = nullptr;
        bool closes_description = false;
        {
            std::lock_guard<std::mutex> lock(runtime_state().mtx);
            current_item = item != nullptr && virtual_bag::valid_index(item_bag) &&
                           item_slot >= 0 && item_slot < virtual_bag::kSlotCount &&
                           runtime_state().objects.module_objects[item_bag][item_slot] == item;
            leader = current_item ? extension_menu_character() : nullptr;
            closes_description = current_item;
        }
        if (leader == nullptr || extension_bag_write_api() == nullptr ||
            extension_bag_write_api()->equipment.use_item == nullptr || !current_item) {
            // 扩展侧没有完整接管前置条件时，direct/confirm 都必须回退原版按钮，
            // 不能清状态或吞掉原版确认弹窗及其 OK 回调。
            (void)extension_desc_call_original(id, button);
            return;
        }
        if (current_item && leader != nullptr && extension_bag_write_api() != nullptr &&
            extension_bag_write_api()->equipment.use_item != nullptr) {
            if (confirm_use) {
                // IsUseAfterConfirm 类物品（如卓越灵药）：原版按钮会弹确认
                // YesNo，其 OK 回调（UIEquip_OKConfrimUseItem）在调
                // CHAR_UseItemEx 前先 INVEN_FindItemSlot 定位——扩展物品不在
                // INVEN 会被拦截而无效果。确认使用函数入口由常驻 LSPosed
                // Native Hook 接管，按回调传入的物品身份分流；此处只让原版
                // 按钮创建确认弹窗。弹窗创建/关闭不持有背包锁。
                extension_bag_ui_description_set_off();
                (void)extension_desc_call_original(id, button);
                return;
            }
            int use_result = 0;
            used = extension_bag_ui_use_item(leader, item, 0, &use_result) && use_result != 0;
        }
        {
            std::lock_guard<std::mutex> lock(runtime_state().mtx);
            if (closes_description) {
                if (used) refresh_module_item_area_locked(item_bag);
                reset_extension_desc_item_hooks_locked();
                runtime_state().ui.extension_desc_item = nullptr;
                runtime_state().ui.extension_desc_item_bag = -1;
                runtime_state().ui.extension_desc_item_slot = -1;
            }
        }
        VIRTBAG_LOG("extension use execute bag=%d slot=%d confirm=%d used=%d", item_bag, item_slot,
                    confirm_use ? 1 : 0, used ? 1 : 0);
        return;
    }

    // 装备页出售（0.7 倍率、只能全卖）走原版销毁链：真实 execute proc 可能是
    // UIEquip_ButtonDestroyExe（创建确认弹窗），也可能是其结算入口
    // UIEquip_OKDestroyItem（0xb83d0）。两者都属目标 proc，任一命中即由模块
    // 接管并走 canonical（下方 item-count facade）；其余 proc 行为不变（非目标
    // proc 直通 call_orig）。
    const bool direct_sell = extension_desc_original_matches(
        id, "desc.button.sell");
    const bool modified_destroy_sell =
        extension_desc_original_matches(id, "desc.button.destroy") ||
        extension_desc_original_matches(id, "desc.button.destroy_ok");
    if (direct_sell || modified_destroy_sell) {
        VIRTBAG_LOG("extension sell button matched source=%s bag=%d slot=%d item=%p",
                    modified_destroy_sell ? "destroy" : "store",
                    runtime_state().ui.extension_desc_item_bag, runtime_state().ui.extension_desc_item_slot,
                    runtime_state().ui.extension_desc_item);
        int64_t price = 0;
        bool valid = false;
        {
            std::lock_guard<std::mutex> lock(runtime_state().mtx);
            const int bag = runtime_state().ui.extension_desc_item_bag;
            const int slot = runtime_state().ui.extension_desc_item_slot;
            void* item = runtime_state().ui.extension_desc_item;
            const bool current_item = item != nullptr && virtual_bag::valid_index(bag) &&
                                      slot >= 0 && slot < virtual_bag::kSlotCount &&
                                      runtime_state().objects.module_objects[bag][slot] == item &&
                                      module_object_release_allowed_locked(
                                          bag, slot, item, "extension sell popup", false, nullptr);
            int category = -1;
            int count = 0;
            int64_t unit_price = -1;
            uint32_t payload = 0;
            uint64_t generation = 0;
            if (current_item && runtime_state().host.game != nullptr &&
                runtime_state().host.item_facts != nullptr && runtime_state().host.item_facts->sell_price != nullptr &&
                runtime_state().host.item_facts->is_no_sell != nullptr) {
        // 卖出数量取模式视图（R-47 决策 b）：启用态 canonical、关闭态 b。
        count = static_cast<int>(virtual_bag_effective_count(item, static_cast<uint32_t>(runtime_state().objects.bag.items[bag][slot].count)));
                int32_t no_sell = 0;
                if (read_item_category(item, &category) &&
                    qol::contracts::succeeded(
                        runtime_state().host.item_facts->is_no_sell(category, &no_sell)) &&
                    no_sell == 0 && count > 0 && extension_bag_ui_read_item_sell_price(item, &unit_price)) {
                    runtime_state().ui.extension_sell_apply_variant_discount = true;
                    price = extension_bag_sell_price(item, count, true);
                    if (price >= 0) {
                        valid = true;
                        const virtual_bag::Item& descriptor = runtime_state().objects.bag.items[bag][slot];
                        payload = virtual_bag::payload_hash(descriptor);
                        generation = runtime_state().objects.module_object_use[bag][slot].generation;
                        const int legal_count = count;
                        QOL_LOG_INFO(QolDomain::kExtensionBag,
                            "sell price source=equip category=%d count=%d unit=%lld variant=%d final=%lld payload=%u gen=%llu",
                            category, legal_count, static_cast<long long>(unit_price),
                            70, static_cast<long long>(price), payload,
                            static_cast<unsigned long long>(generation));
                    } else {
                        QOL_LOG_WARN(QolDomain::kExtensionBag,
                            "sell price reject reason=price_bounds source=equip category=%d count=%d unit=%lld variant=%d payload=%u gen=%llu",
                            category, count, static_cast<long long>(unit_price),
                            70, payload,
                            static_cast<unsigned long long>(runtime_state().objects.module_object_use[bag][slot].generation));
                    }
                } else {
                    QOL_LOG_WARN(QolDomain::kExtensionBag,
                        "sell price reject reason=no_sell_or_count source=equip category=%d count=%d variant=%d",
                        category, count, 70);
                }
            } else if (!current_item) {
                QOL_LOG_WARN(QolDomain::kExtensionBag, "sell price reject reason=stale_or_release_guard source=equip bag=%d slot=%d",
                            bag, slot);
            } else {
                QOL_LOG_WARN(QolDomain::kExtensionBag, "sell price reject reason=symbols source=equip bag=%d slot=%d",
                            bag, slot);
            }
            runtime_state().ui.extension_sell_original_ok = nullptr;
            runtime_state().ui.extension_sell_original_cancel = nullptr;
        }
        const auto* ui_write = runtime_state().host.game == nullptr ? nullptr : &runtime_state().host.write_port->ui;
        bool popup_created = valid && price <= UINT32_MAX && ui_write != nullptr &&
                             ui_write->create_confirmation != nullptr;
        if (popup_created) {
            // 原版按钮先关闭详情再创建确认弹窗；保持同样的 UI 生命周期，且不持有背包锁调用游戏函数。
            extension_bag_ui_description_set_off();
            const QolResult result = ui_write->create_confirmation(
                modified_destroy_sell ? 1 : 0, static_cast<uint32_t>(price),
                reinterpret_cast<void*>(&extension_sell_ok),
                reinterpret_cast<void*>(&extension_sell_cancel),
                reinterpret_cast<void**>(&runtime_state().ui.extension_sell_original_ok),
                reinterpret_cast<void**>(&runtime_state().ui.extension_sell_original_cancel));
            if (!qol::contracts::succeeded(result)) {
                VIRTBAG_LOG("extension sell confirmation unavailable code=%d", result.code);
                runtime_state().ui.extension_sell_original_ok = nullptr;
                runtime_state().ui.extension_sell_original_cancel = nullptr;
                popup_created = false;
            }
            if (popup_created) {
                VIRTBAG_LOG("extension sell confirmation created price=%lld", static_cast<long long>(price));
            }
        } else {
            VIRTBAG_LOG("sell price reject reason=popup_unavailable source=equip valid=%d",
                        valid ? 1 : 0);
        }
        std::lock_guard<std::mutex> lock(runtime_state().mtx);
        if (popup_created) {
            reset_extension_desc_item_hooks_locked();
        } else {
            clear_original_desc_locked();
        }
        return;
    }
    // 未命中任何目标 proc 时回退原版；item 为分流块锁内捕获的本地副本。
    if (item != nullptr) {
        VIRTBAG_LOG("extension desc execute unmatched index=%d original=%p bag=%d slot=%d",
                    hook_index, original, item_bag, item_slot);
    }
    (void)extension_desc_call_original(id, button);

    std::lock_guard<std::mutex> lock(runtime_state().mtx);
    if (item == nullptr || !virtual_bag::valid_index(item_bag) ||
        item_slot < 0 || item_slot >= virtual_bag::kSlotCount ||
        runtime_state().objects.module_objects[item_bag][item_slot] != item) {
        reset_extension_desc_item_hooks_locked();
        return;
    }
    std::array<uint8_t, virtual_bag::kSerializedItemBuffer> payload{};
    int payload_size = 0;
    if (!serialize_item_payload_locked(item, &payload, &payload_size)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "extension desc operation reserialize failed bag=%d slot=%d", item_bag,
                    item_slot);
        reset_extension_desc_item_hooks_locked();
        return;
    }
    virtual_bag::Item& descriptor = runtime_state().objects.bag.items[item_bag][item_slot];
    const virtual_bag::Item previous_descriptor = descriptor;
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    (void)read_item_category(item, &descriptor.category);
    // descriptor.count 恒为 canonical（R-38/R-48）：getter 在关闭态只返回 b 视图
    //（R-47 决策 b），物化/重同步必须保存完整 128a+b。
    if (extension_bag_ui_read_item_cumulate_count(item, &descriptor.count)) {
        descriptor.count = static_cast<int>(virtual_bag_canonical_count(item, 1));
    }
    if (descriptor.category <= 0 || descriptor.count <= 0) {
        descriptor = {};
        if (!free_module_object_locked(item_bag, item_slot)) {
            descriptor = previous_descriptor;
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension desc sync rejected object release bag=%d slot=%d",
                        item_bag, item_slot);
            reset_extension_desc_item_hooks_locked();
            return;
        }
    } else {
        runtime_state().objects.module_object_categories[item_bag][item_slot] = descriptor.category;
        runtime_state().objects.module_object_hashes[item_bag][item_slot] = virtual_bag::payload_hash(descriptor);
    }
    extension_bag_mark_dirty_locked();
    reset_extension_desc_item_hooks_locked();
    QOL_LOG_INFO(QolDomain::kExtensionBag, "extension desc operation synced bag=%d slot=%d category=%d count=%d",
                item_bag, item_slot, descriptor.category, descriptor.count);
}
