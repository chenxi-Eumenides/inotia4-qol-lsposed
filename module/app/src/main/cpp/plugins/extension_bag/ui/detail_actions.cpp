#include "plugins/extension_bag/transaction/transaction.h"
#include "plugins/extension_bag/api/bag_source.h"
#include "plugins/extension_bag/core/ui_state.h"
#include "game_ui_virtbag.h"
#include "plugins/extension_bag/runtime/state_access.h"
#include "plugins/extension_bag/runtime/reentry_lock.h"
#include "plugins/extension_bag/runtime/inventory_access.h"
#include "plugins/extension_bag/runtime/item_access.h"
#include "plugins/extension_bag/runtime/inventory_action.h"
#include "plugins/extension_bag/core/lifecycle.h"
#include "plugins/extension_bag/runtime/ownership_port.h"
#include "plugins/extension_bag/core/projection.h"
#include "plugins/extension_bag/runtime/shared.h"
#include "plugins/extension_bag/core/tabs.h"
#include "plugins/extension_bag/core/host_adapter.h"
#include "plugins/extension_bag/ui/host_ui.h"
#include "contracts/game_host.h"
#include "contracts/host_sdk.h"
#include "contracts/ui.h"
#include "contracts/ui_confirmation.h"
#include "contracts/ownership.h"
#include "contracts/inventory_refresh.h"
#include "contracts/item_facts.h"
#include "contracts/sell_price.h"
#include "contracts/module_save_port.h"
#include "contracts/popup_enter_alias.h"
#include "plugins/extension_bag/model/equip_rules.h"
#include "plugins/extension_bag/core/geometry.h"
#include "plugins/extension_bag/runtime/logging.h"
#include "plugins/extension_bag/core/move_merge.h"
#include "plugins/extension_bag/model/ownership_ledger.h"
#include "plugins/extension_bag/model/item_handle_custody.h"
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

bool g_extension_custom_sell_confirmation = false;

const qol::contracts::UiConfirmationApi* extension_ui_confirmation_api() {
    const auto* sdk = ::qol::extension_bag::access::host().sdk;
    if (sdk == nullptr || sdk->capabilities.require == nullptr) return nullptr;
    const void* iface = nullptr;
    const QolResult result = sdk->capabilities.require(
        qol::contracts::kUiConfirmationCapabilityId,
        {static_cast<uint16_t>(qol::contracts::kUiConfirmationVersionMajor),
         static_cast<uint16_t>(qol::contracts::kUiConfirmationVersionMinor)},
        &iface);
    const auto* api = qol::contracts::succeeded(result)
                          ? static_cast<const qol::contracts::UiConfirmationApi*>(iface)
                          : nullptr;
    return api != nullptr && api->open_text != nullptr ? api : nullptr;
}

bool extension_open_custom_sell_confirmation(uint32_t price) {
    const auto* api = extension_ui_confirmation_api();
    if (api == nullptr) return false;
    qol::contracts::UiConfirmationSpecText spec{};
    spec.size = sizeof(spec);
    std::snprintf(spec.message, sizeof(spec.message), "确定以 %u 金币出售该物品？", price);
    spec.price = price;
    spec.on_confirm = &extension_sell_ok;
    spec.on_cancel = &extension_sell_cancel;
    spec.user = nullptr;
    uint64_t token = 0;
    const QolResult result = api->open_text(&spec, &token);
    if (!qol::contracts::succeeded(result) || token == 0) {
        VIRTBAG_LOG("extension sell custom confirmation unavailable code=%d token=%llu",
                    result.code, static_cast<unsigned long long>(token));
        if (token != 0 && api->close != nullptr) (void)api->close(token);
        return false;
    }
    g_extension_custom_sell_confirmation = true;
    VIRTBAG_LOG("extension sell custom confirmation opened price=%u token=%llu", price,
                static_cast<unsigned long long>(token));
    return true;
}

bool extension_detail_sell_quote(void* item, int count, int64_t* out_price,
                                 int64_t* out_unit_price, uint32_t* out_num,
                                 uint32_t* out_den) {
    if (item == nullptr || count <= 0 || out_price == nullptr) return false;
    const auto* facts = ::qol::extension_bag::access::host().item_facts;
    if (facts == nullptr || facts->quote == nullptr ||
        !qol::contracts::item_facts_api_quote_size_valid(facts)) {
        return false;
    }
    qol::contracts::QolItemHandle handle{};
    if (!extension_bag_item_handle_for_pointer(item, &handle)) return false;
    qol::contracts::QolSaleQuote quote{};
    quote.struct_size = sizeof(quote);
    if (!qol::contracts::succeeded(facts->quote(handle, qol::contracts::kDetailSell, &quote)) ||
        quote.unit_price < 0 || quote.multiplier_den == 0 ||
        quote.multiplier_num > quote.multiplier_den) {
        return false;
    }
    int64_t price = 0;
    if (!qol::contracts::sell_price::calculate(
            quote.unit_price, static_cast<uint32_t>(count), quote.multiplier_num,
            quote.multiplier_den, &price)) {
        return false;
    }
    QOL_LOG_INFO(QolDomain::kExtensionBag,
                 "sell quote item=%p count=%d unit=%lld multiplier=%u/%u final=%lld",
                 item, count, static_cast<long long>(quote.unit_price), quote.multiplier_num,
                 quote.multiplier_den, static_cast<long long>(price));
    *out_price = price;
    if (out_unit_price != nullptr) *out_unit_price = quote.unit_price;
    if (out_num != nullptr) *out_num = quote.multiplier_num;
    if (out_den != nullptr) *out_den = quote.multiplier_den;
    return true;
}

}

void restore_extension_desc_popup_callbacks_locked() {
    if (::qol::extension_bag::access::host().game != nullptr &&
        ::qol::extension_bag::access::host().write_port->ui.restore_confirmation != nullptr) {
        (void)::qol::extension_bag::access::host().write_port->ui.restore_confirmation(
            1, reinterpret_cast<void*>(&extension_destroy_ok),
            reinterpret_cast<void*>(&extension_destroy_cancel),
            reinterpret_cast<void*>(qol::extension_bag::ui_state().ui.extension_destroy_original_ok),
            reinterpret_cast<void*>(qol::extension_bag::ui_state().ui.extension_destroy_original_cancel));
        (void)::qol::extension_bag::access::host().write_port->ui.restore_confirmation(
            0, reinterpret_cast<void*>(&extension_sell_ok),
            reinterpret_cast<void*>(&extension_sell_cancel),
            reinterpret_cast<void*>(qol::extension_bag::ui_state().ui.extension_sell_original_ok),
            reinterpret_cast<void*>(qol::extension_bag::ui_state().ui.extension_sell_original_cancel));
    }
    qol::extension_bag::ui_state().ui.extension_destroy_original_ok = nullptr;
    qol::extension_bag::ui_state().ui.extension_sell_original_ok = nullptr;
    qol::extension_bag::ui_state().ui.extension_destroy_original_cancel = nullptr;
    qol::extension_bag::ui_state().ui.extension_sell_original_cancel = nullptr;
    qol::extension_bag::ui_state().ui.extension_sell_apply_variant_discount = true;
}

namespace {

void extension_sell_cancel(void*) {
    ExtensionDestroyOkFn original_cancel = nullptr;
    {
        ::qol::extension_bag::ReentryLock lock;
        const bool custom_confirmation = g_extension_custom_sell_confirmation;
        g_extension_custom_sell_confirmation = false;
        original_cancel = qol::extension_bag::ui_state().ui.extension_sell_original_cancel;
        if (!custom_confirmation) restore_extension_desc_popup_callbacks_locked();
        reset_extension_desc_item_hooks_locked();
        qol::extension_bag::ui_state().ui.extension_desc_item = nullptr;
        qol::extension_bag::ui_state().ui.extension_desc_item_bag = -1;
        qol::extension_bag::ui_state().ui.extension_desc_item_slot = -1;
    }
    extension_bag_ui_description_set_off();
    if (original_cancel != nullptr) original_cancel(nullptr);
}

void extension_destroy_cancel(void*) {
    ExtensionDestroyOkFn original_cancel = nullptr;
    {
        ::qol::extension_bag::ReentryLock lock;
        original_cancel = qol::extension_bag::ui_state().ui.extension_destroy_original_cancel;
        restore_extension_desc_popup_callbacks_locked();
        reset_extension_desc_item_hooks_locked();
        qol::extension_bag::ui_state().ui.extension_desc_item = nullptr;
        qol::extension_bag::ui_state().ui.extension_desc_item_bag = -1;
        qol::extension_bag::ui_state().ui.extension_desc_item_slot = -1;
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
    qol::contracts::QolItemHandle expected_item{};
    {
        ::qol::extension_bag::ReentryLock lock;
        const bool custom_confirmation = g_extension_custom_sell_confirmation;
        g_extension_custom_sell_confirmation = false;
        if (!custom_confirmation) restore_extension_desc_popup_callbacks_locked();
        item = qol::extension_bag::ui_state().ui.extension_desc_item;
        bag = qol::extension_bag::ui_state().ui.extension_desc_item_bag;
        slot = qol::extension_bag::ui_state().ui.extension_desc_item_slot;
        const bool current_item = item != nullptr && virtual_bag::valid_index(bag) &&
                                  slot >= 0 && slot < virtual_bag::kSlotCount &&
                                  qol::extension_bag::ui_state().objects.module_objects[bag][slot] == item &&
                                  module_object_release_allowed_locked(
                                      bag, slot, item, "extension sell", false, nullptr);
        if (!current_item) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension sell rejected stale detail bag=%d slot=%d item=%p", bag,
                        slot, item);
            reset_extension_desc_item_hooks_locked();
            qol::extension_bag::ui_state().ui.extension_desc_item = nullptr;
            qol::extension_bag::ui_state().ui.extension_desc_item_bag = -1;
            qol::extension_bag::ui_state().ui.extension_desc_item_slot = -1;
            clear_original_desc_locked();
            return;
        }
        if (!extension_bag_item_handle_for_pointer(item, &expected_item)) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension sell rejected item handle unavailable bag=%d slot=%d", bag, slot);
            clear_original_desc_locked();
            return;
        }
        if (::qol::extension_bag::access::host().game == nullptr ||
            ::qol::extension_bag::access::host().item_facts == nullptr ||
            ::qol::extension_bag::access::host().item_facts->sell_price == nullptr ||
            ::qol::extension_bag::access::host().item_facts->is_no_sell == nullptr) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension sell rejected symbols unavailable bag=%d slot=%d", bag, slot);
            clear_original_desc_locked();
            return;
        }
                // 出售按 sidecar 的 canonical 数量结算。装备等非堆叠物品的
                // item 对象 count 字段可能不是可视数量（曾见 equipment=100）。
                count = ::qol::extension_bag::access::bag().items[bag][slot].count;
        int category = -1;
        int32_t no_sell = 0;
        if (!read_item_category(item, &category) ||
            !qol::contracts::succeeded(::qol::extension_bag::access::host().item_facts->is_no_sell(category, &no_sell)) ||
            no_sell != 0) {
            QOL_LOG_WARN(QolDomain::kExtensionBag, "extension sell rejected no-sell item bag=%d slot=%d category=%d",
                        bag, slot, category);
            clear_original_desc_locked();
            return;
        }
        int64_t unit_price = 0;
        uint32_t multiplier_num = 0;
        uint32_t multiplier_den = 0;
        if (!extension_detail_sell_quote(item, count, &price, &unit_price, &multiplier_num,
                                         &multiplier_den)) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                "sell price reject reason=quote source=equip category=%d count=%d payload=%u gen=%llu",
                category, count,
                virtual_bag::payload_hash(::qol::extension_bag::access::bag().items[bag][slot]),
                static_cast<unsigned long long>(qol::extension_bag::ui_state().objects.module_object_use[bag][slot].generation));
            clear_original_desc_locked();
            return;
        }
        QOL_LOG_INFO(QolDomain::kExtensionBag,
            "sell price source=equip category=%d count=%d unit=%lld multiplier=%u/%u final=%lld payload=%u gen=%llu",
            category, count, static_cast<long long>(unit_price), multiplier_num, multiplier_den,
            static_cast<long long>(price),
            virtual_bag::payload_hash(::qol::extension_bag::access::bag().items[bag][slot]),
            static_cast<unsigned long long>(qol::extension_bag::ui_state().objects.module_object_use[bag][slot].generation));
    }

    int64_t money_before = -1;
    int64_t money_after = -1;
    const auto* facts = ::qol::extension_bag::access::host().item_facts;
    if (facts != nullptr && facts->money != nullptr) {
        (void)facts->money(&money_before);
    }
    const auto* action = ::qol::features::extension_bag::inventory_action_api();
    if (action == nullptr) {
        ::qol::extension_bag::ReentryLock lock;
        clear_original_desc_locked();
        return;
    }
    const auto result = action->sell({qol::features::extension_bag::kExtensionBagSpaceId, bag, slot}, expected_item, count, "equip");
    if (facts != nullptr && facts->money != nullptr) {
        (void)facts->money(&money_after);
    }
    if (result.status != qol::contracts::InventorySellStatus::kOk) {
        VIRTBAG_LOG("extension sell action rejected bag=%d slot=%d count=%d status=%d money=%lld->%lld",
                    bag, slot, count, static_cast<int>(result.status),
                    static_cast<long long>(money_before), static_cast<long long>(money_after));
        ::qol::extension_bag::ReentryLock lock;
        clear_original_desc_locked();
        return;
    }
    price = result.price;
    QOL_LOG_INFO(QolDomain::kExtensionBag,
                 "extension sell action ok bag=%d slot=%d count=%d action_price=%lld money=%lld->%lld",
                 bag, slot, count, static_cast<long long>(result.price),
                 static_cast<long long>(money_before), static_cast<long long>(money_after));

    {
        ::qol::extension_bag::ReentryLock lock;
        reset_extension_desc_item_hooks_locked();
        qol::extension_bag::ui_state().ui.extension_desc_item = nullptr;
        qol::extension_bag::ui_state().ui.extension_desc_item_bag = -1;
        qol::extension_bag::ui_state().ui.extension_desc_item_slot = -1;
        clear_original_desc_locked();
        // 出售已从 sidecar 删除源物品；立即重绑投影，避免控件仍显示旧物品。
        refresh_module_item_area_locked(bag);
    }
    VIRTBAG_LOG("extension sell committed bag=%d slot=%d price=%lld", bag, slot,
                static_cast<long long>(price));
}

void extension_destroy_ok(void*) {
    int bag = -1;
    int slot = -1;
    int count = 0;
    {
        ::qol::extension_bag::ReentryLock lock;
        restore_extension_desc_popup_callbacks_locked();

        bag = qol::extension_bag::ui_state().ui.extension_desc_item_bag;
        slot = qol::extension_bag::ui_state().ui.extension_desc_item_slot;
        void* item = qol::extension_bag::ui_state().ui.extension_desc_item;
        const bool valid_item = item != nullptr && virtual_bag::valid_index(bag) &&
            slot >= 0 && slot < virtual_bag::kSlotCount &&
            qol::extension_bag::ui_state().objects.module_objects[bag][slot] == item &&
            module_object_release_allowed_locked(bag, slot, item, "extension destroy", false, nullptr);
        if (valid_item) {
            count = ::qol::extension_bag::access::bag().items[bag][slot].count;
        } else {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension destroy rejected stale detail bag=%d slot=%d", bag, slot);
        }
    }
    const auto* action = ::qol::features::extension_bag::inventory_action_api();
    bool committed = false;
    if (action != nullptr && count > 0) {
        const auto result = action->discard({qol::features::extension_bag::kExtensionBagSpaceId, bag, slot}, count);
        committed = result.status == qol::contracts::InventoryActionStatus::kOk;
        if (!committed) {
            VIRTBAG_LOG("extension destroy action rejected bag=%d slot=%d count=%d status=%d",
                        bag, slot, count, static_cast<int>(result.status));
        }
    }
    {
        ::qol::extension_bag::ReentryLock lock;
        reset_extension_desc_item_hooks_locked();
        qol::extension_bag::ui_state().ui.extension_desc_item = nullptr;
        qol::extension_bag::ui_state().ui.extension_desc_item_bag = -1;
        qol::extension_bag::ui_state().ui.extension_desc_item_slot = -1;
        clear_original_desc_locked();
    }
    if (committed) {
        VIRTBAG_LOG("extension destroy committed through inventory.action bag=%d slot=%d", bag, slot);
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
        const virtual_bag::Item previous_item = ::qol::extension_bag::access::bag().items[bag][slot];
        ::qol::extension_bag::access::bag().items[bag][slot] = {};
        if (!free_module_object_locked(bag, slot, raw_use_token)) {
            ::qol::extension_bag::access::bag().items[bag][slot] = previous_item;
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
    virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
    descriptor.payload = payload;
    descriptor.payload_size = payload_size;
    // descriptor.count 恒为 canonical：关闭态 b 写保留 a，重开读回完整值。
    descriptor.count = static_cast<int>(virtual_bag_canonical_count(
        item, virtual_bag_canonical_from_field(old_count_flags,
                                               static_cast<uint32_t>(descriptor.count))));
    qol::extension_bag::ui_state().objects.module_object_categories[bag][slot] = descriptor.category;
    qol::extension_bag::ui_state().objects.module_object_hashes[bag][slot] = virtual_bag::payload_hash(descriptor);
    // ApplyStuff consumes a stacked material through H-04. Keep the token's
    // consumed bit even when the native object remains live, otherwise the
    // caller treats a successful jewel/scroll apply as an unconsumed failure.
    if (raw_use_token != nullptr) {
        ModuleUseToken* token = static_cast<ModuleUseToken*>(raw_use_token);
        ModuleUseState& use = qol::extension_bag::ui_state().objects.module_object_use[bag][slot];
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
    int bag = -1;
    int slot = -1;
    {
        ::qol::extension_bag::TryReentryLock lock;
        if (!lock.owns_lock()) {
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension confirm-use lock busy item=%p", item);
            return false;
        }
        if (!extension_bag_enabled() || item == nullptr ||
            item != qol::extension_bag::ui_state().ui.extension_desc_item) {
            return false;
        }
        bag = qol::extension_bag::ui_state().ui.extension_desc_item_bag;
        slot = qol::extension_bag::ui_state().ui.extension_desc_item_slot;
        int module_bag = -1;
        int module_slot = -1;
        if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
            ::qol::extension_bag::access::bag().items[bag][slot].category <= 0 ||
            ::qol::extension_bag::access::bag().items[bag][slot].count <= 0 ||
            qol::extension_bag::ui_state().objects.module_objects[bag][slot] != item ||
            !module_slot_of_item_locked(item, &module_bag, &module_slot) ||
            module_bag != bag || module_slot != slot) {
            return false;
        }
    }

    const auto* action = ::qol::features::extension_bag::inventory_action_api();
    bool used = false;
    if (action != nullptr) {
        const auto outcome = action->use({qol::features::extension_bag::kExtensionBagSpaceId, bag, slot});
        used = outcome.status == qol::contracts::InventoryActionStatus::kOk;
        if (!used) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension confirm-use action rejected bag=%d slot=%d status=%d",
                         bag, slot, static_cast<int>(outcome.status));
        }
    }
    {
        ::qol::extension_bag::ReentryLock lock;
        if (qol::extension_bag::ui_state().ui.extension_desc_item == item) {
            if (used && virtual_bag::valid_index(bag)) refresh_module_item_area_locked(bag);
            reset_extension_desc_item_hooks_locked();
            qol::extension_bag::ui_state().ui.extension_desc_item = nullptr;
            qol::extension_bag::ui_state().ui.extension_desc_item_bag = -1;
            qol::extension_bag::ui_state().ui.extension_desc_item_slot = -1;
        }
    }
    VIRTBAG_LOG("extension confirm-use inventory.action bag=%d slot=%d used=%d",
                bag, slot, used ? 1 : 0);
    return true;
}

// R-63 佣兵徽章（category 42..50 / 928..933）使用按钮函数级接管。
// 原版 UIEquip_ButtonUseMercenarySealExe(0xb8144) 经 ControlItem_GetItem 从 g_inven
// 取物品，扩展物品不进物理袋会取到 null/错位而静默失败。此处按原版流程用模块详情
// 身份直接调原版 MERCENARYSYSTEM_MakeMercenary（0x119658），消耗交给已 Hook 的
// INVEN_RemoveItem（H-04）承接扩展槽删除；SAVE_IsOK/空槽校验顺序与原版一致。
// 返回 true=已接管（原版按钮不再执行）；false=非扩展详情佣兵徽章，交原版透传。
bool extension_use_mercenary_seal() {
    int bag = -1;
    int slot = -1;
    {
        ::qol::extension_bag::TryReentryLock lock;
        if (!lock.owns_lock()) {
            QOL_LOG_INFO(QolDomain::kExtensionBag, "extension mercenary-seal lock busy");
            return false;
        }
        if (!extension_bag_enabled() ||
            qol::extension_bag::ui_state().ui.extension_desc_item == nullptr) {
            return false;
        }
        void* item = qol::extension_bag::ui_state().ui.extension_desc_item;
        bag = qol::extension_bag::ui_state().ui.extension_desc_item_bag;
        slot = qol::extension_bag::ui_state().ui.extension_desc_item_slot;
        if (!virtual_bag::valid_index(bag) || slot < 0 || slot >= virtual_bag::kSlotCount ||
            qol::extension_bag::ui_state().objects.module_objects[bag][slot] != item ||
            ::qol::extension_bag::access::bag().items[bag][slot].category <= 0 ||
            ::qol::extension_bag::access::bag().items[bag][slot].count <= 0) {
            return false;
        }
        int category = ::qol::extension_bag::access::bag().items[bag][slot].category;
        (void)read_item_category(item, &category);
        if (!read_item_is_mercenary_seal(category)) return false;
    }

    const auto* action = ::qol::features::extension_bag::inventory_action_api();
    bool used = false;
    if (action != nullptr) {
        const auto outcome = action->use({qol::features::extension_bag::kExtensionBagSpaceId, bag, slot});
        used = outcome.status == qol::contracts::InventoryActionStatus::kOk;
        if (!used) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension mercenary-seal action rejected bag=%d slot=%d status=%d",
                         bag, slot, static_cast<int>(outcome.status));
        }
    }
    {
        ::qol::extension_bag::ReentryLock lock;
        reset_extension_desc_item_hooks_locked();
        qol::extension_bag::ui_state().ui.extension_desc_item = nullptr;
        qol::extension_bag::ui_state().ui.extension_desc_item_bag = -1;
        qol::extension_bag::ui_state().ui.extension_desc_item_slot = -1;
    }
    if (::qol::extension_bag::access::host().sdk != nullptr &&
        ::qol::extension_bag::access::host().sdk->ui != nullptr &&
        ::qol::extension_bag::access::host().sdk->ui->show_text_popup != nullptr) {
        (void)::qol::extension_bag::access::host().sdk->ui->show_text_popup(
            used ? 0x18 : 0xc);
    }
    VIRTBAG_LOG("extension mercenary-seal inventory.action bag=%d slot=%d used=%d",
                bag, slot, used ? 1 : 0);
    return true;
}

void extension_desc_item_execute_at(size_t index, void* button) {
    if (index >= qol::extension_bag::ui_state().ui.extension_desc_item_bound.size() ||
        !qol::extension_bag::ui_state().ui.extension_desc_item_bound[index] || button == nullptr) return;
    const uint64_t id = qol::extension_bag::ui_state().ui.extension_desc_item_ids[index];
    const auto* api = qol::contracts::desc_hook_v2_if_available(::qol::extension_bag::access::host().sdk);
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
        ::qol::extension_bag::ReentryLock lock;
        item = qol::extension_bag::ui_state().ui.extension_desc_item;
        item_bag = qol::extension_bag::ui_state().ui.extension_desc_item_bag;
        item_slot = qol::extension_bag::ui_state().ui.extension_desc_item_slot;
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
        bool closes_description = false;
        {
            ::qol::extension_bag::ReentryLock lock;
            current_item = item != nullptr && virtual_bag::valid_index(item_bag) &&
                           item_slot >= 0 && item_slot < virtual_bag::kSlotCount &&
                           qol::extension_bag::ui_state().objects.module_objects[item_bag][item_slot] == item;
            closes_description = current_item;
        }
        if (!current_item) {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension use rejected stale detail bag=%d slot=%d", item_bag, item_slot);
            return;
        }
        if (confirm_use) {
            // 原版按钮需要当前详情上下文来创建确认弹窗；确认回调自身用
            // inventory.action.use 结算，不能先关闭详情。
            (void)extension_desc_call_original(id, button);
            return;
        }
        const auto* action = ::qol::features::extension_bag::inventory_action_api();
        if (action != nullptr) {
            const auto outcome = action->use({qol::features::extension_bag::kExtensionBagSpaceId, item_bag, item_slot});
            used = outcome.status == qol::contracts::InventoryActionStatus::kOk;
            if (!used) {
                QOL_LOG_WARN(QolDomain::kExtensionBag,
                             "extension use action rejected bag=%d slot=%d status=%d",
                             item_bag, item_slot, static_cast<int>(outcome.status));
            }
        }
        {
            ::qol::extension_bag::ReentryLock lock;
            if (closes_description) {
                if (used) refresh_module_item_area_locked(item_bag);
                reset_extension_desc_item_hooks_locked();
                qol::extension_bag::ui_state().ui.extension_desc_item = nullptr;
                qol::extension_bag::ui_state().ui.extension_desc_item_bag = -1;
                qol::extension_bag::ui_state().ui.extension_desc_item_slot = -1;
            }
        }
        VIRTBAG_LOG("extension use execute bag=%d slot=%d confirm=%d used=%d", item_bag, item_slot,
                    confirm_use ? 1 : 0, used ? 1 : 0);
        return;
    }

    // 解封按钮仍由原版执行物品类别分派与产物入库；扩展物品的消耗经
    // INVEN_ConsumeItem/QolBagSource::remove 回到 sidecar，不能在这里本地改数据。
    if (extension_desc_original_matches(id, "desc.button.release_sealed")) {
        bool current_item = false;
        {
            ::qol::extension_bag::ReentryLock lock;
            current_item = item != nullptr && virtual_bag::valid_index(item_bag) &&
                           item_slot >= 0 && item_slot < virtual_bag::kSlotCount &&
                           qol::extension_bag::ui_state().objects.module_objects[item_bag][item_slot] == item;
        }
        if (current_item) {
            (void)extension_desc_call_original(id, button);
        } else {
            QOL_LOG_WARN(QolDomain::kExtensionBag,
                         "extension release-sealed rejected stale detail bag=%d slot=%d",
                         item_bag, item_slot);
        }
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
        // 商店页出售必须走 UIStore 的原版数量/确认链；背包页详情出售才归
        // extension_bag 接管。两者共用 desc 按钮槽，因此按商店场景分流。
        if (qol::extension_bag::ui_state().store.frame_active || qol::extension_bag::ui_state().store.view_installed) {
            (void)extension_desc_call_original(id, button);
            return;
        }
        VIRTBAG_LOG("extension sell button matched source=%s bag=%d slot=%d item=%p",
                    modified_destroy_sell ? "destroy" : "store",
                    qol::extension_bag::ui_state().ui.extension_desc_item_bag, qol::extension_bag::ui_state().ui.extension_desc_item_slot,
                    qol::extension_bag::ui_state().ui.extension_desc_item);
        int64_t price = 0;
        bool valid = false;
        {
            ::qol::extension_bag::ReentryLock lock;
            const int bag = qol::extension_bag::ui_state().ui.extension_desc_item_bag;
            const int slot = qol::extension_bag::ui_state().ui.extension_desc_item_slot;
            void* item = qol::extension_bag::ui_state().ui.extension_desc_item;
            const bool current_item = item != nullptr && virtual_bag::valid_index(bag) &&
                                      slot >= 0 && slot < virtual_bag::kSlotCount &&
                                      qol::extension_bag::ui_state().objects.module_objects[bag][slot] == item &&
                                      module_object_release_allowed_locked(
                                          bag, slot, item, "extension sell popup", false, nullptr);
            int category = -1;
            int count = 0;
            int64_t unit_price = -1;
            uint32_t multiplier_num = 0;
            uint32_t multiplier_den = 0;
            uint32_t payload = 0;
            uint64_t generation = 0;
            if (current_item && ::qol::extension_bag::access::host().game != nullptr &&
                ::qol::extension_bag::access::host().item_facts != nullptr &&
                ::qol::extension_bag::access::host().item_facts->is_no_sell != nullptr) {
                // 出售按 sidecar 的 canonical 数量结算。装备等非堆叠物品的
                // item 对象 count 字段可能不是可视数量（曾见 equipment=100）。
                count = ::qol::extension_bag::access::bag().items[bag][slot].count;
                int32_t no_sell = 0;
                if (read_item_category(item, &category) &&
                    qol::contracts::succeeded(
                        ::qol::extension_bag::access::host().item_facts->is_no_sell(category, &no_sell)) &&
                    no_sell == 0 && count > 0 &&
                    extension_detail_sell_quote(item, count, &price, &unit_price,
                                                &multiplier_num, &multiplier_den)) {
                    valid = true;
                    const virtual_bag::Item& descriptor = ::qol::extension_bag::access::bag().items[bag][slot];
                    payload = virtual_bag::payload_hash(descriptor);
                    generation = qol::extension_bag::ui_state().objects.module_object_use[bag][slot].generation;
                    QOL_LOG_INFO(QolDomain::kExtensionBag,
                        "sell price source=equip category=%d count=%d unit=%lld multiplier=%u/%u final=%lld payload=%u gen=%llu",
                        category, count, static_cast<long long>(unit_price), multiplier_num,
                        multiplier_den, static_cast<long long>(price), payload,
                        static_cast<unsigned long long>(generation));
                } else {
                    QOL_LOG_WARN(QolDomain::kExtensionBag,
                        "sell price reject reason=quote_or_no_sell source=equip category=%d count=%d",
                        category, count);
                }
            } else if (!current_item) {
                QOL_LOG_WARN(QolDomain::kExtensionBag, "sell price reject reason=stale_or_release_guard source=equip bag=%d slot=%d",
                            bag, slot);
            } else {
                QOL_LOG_WARN(QolDomain::kExtensionBag, "sell price reject reason=symbols source=equip bag=%d slot=%d",
                            bag, slot);
            }
            qol::extension_bag::ui_state().ui.extension_sell_original_ok = nullptr;
            qol::extension_bag::ui_state().ui.extension_sell_original_cancel = nullptr;
        }
        bool popup_created = false;
        if (valid && price <= UINT32_MAX) {
            // 只使用 core.custom_ui 的原始文本确认框。回退原版销毁弹窗会在某些
            // 场景显示默认文本/错误价格，且不会删除扩展袋 sidecar 物品。
            extension_bag_ui_description_set_off();
            popup_created = extension_open_custom_sell_confirmation(static_cast<uint32_t>(price));
            if (!popup_created) {
                VIRTBAG_LOG("extension sell custom confirmation unavailable; original fallback disabled");
                const auto* sdk = ::qol::extension_bag::access::host().sdk;
                if (sdk != nullptr && sdk->ui != nullptr && sdk->ui->show_text_popup != nullptr) {
                    (void)sdk->ui->show_text_popup(0xc);
                }
            } else {
                VIRTBAG_LOG("extension sell confirmation created price=%lld custom=1",
                            static_cast<long long>(price));
            }
        } else {
            VIRTBAG_LOG("sell price reject reason=popup_unavailable source=equip valid=%d",
                        valid ? 1 : 0);
        }
        ::qol::extension_bag::ReentryLock lock;
        if (popup_created) {
            reset_extension_desc_item_hooks_locked();
        } else {
            clear_original_desc_locked();
        }
        return;
    }
    // 未识别操作没有 inventory.action 入口时必须拒绝，不能让原版直接改写 sidecar 对象。
    VIRTBAG_LOG("extension desc execute rejected unsupported action index=%d original=%p bag=%d slot=%d",
                hook_index, original, item_bag, item_slot);
}
