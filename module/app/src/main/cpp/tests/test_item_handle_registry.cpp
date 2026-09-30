#include "game/access/item_handle_registry.h"

#include <cassert>
#include <cstdint>
#include <mutex>

namespace {

bool g_main_thread = true;
bool is_main_thread() { return g_main_thread; }
qol::game::ItemHandleRegistry* g_callback_registry = nullptr;
std::mutex g_virtual_bag_mtx;
bool g_virtual_bag_lock_held = false;
int g_refresh_calls = 0;
int g_callback_calls = 0;

int32_t register_locked(void* item, uint32_t provider, qol::contracts::QolItemHandle* out) {
    assert(g_virtual_bag_lock_held);
    assert(out != nullptr);
    ++g_callback_calls;
    *out = g_callback_registry->register_item(item, provider);
    return out->slot != 0 ? qol::contracts::kQolItemHandleStatusOk
                          : qol::contracts::kQolItemHandleStatusCapacity;
}

int32_t invalidate_locked(void* item, uint32_t provider) {
    assert(g_virtual_bag_lock_held);
    ++g_callback_calls;
    return g_callback_registry->invalidate_item(item, provider)
               ? qol::contracts::kQolItemHandleStatusOk
               : qol::contracts::kQolItemHandleStatusNotFound;
}

int32_t rebind_locked(void* item, uint32_t provider) {
    assert(g_virtual_bag_lock_held);
    ++g_callback_calls;
    return g_callback_registry->rebind_item(item, provider)
               ? qol::contracts::kQolItemHandleStatusOk
               : qol::contracts::kQolItemHandleStatusNotFound;
}

int32_t begin_invalidation_locked(void* item, uint32_t provider,
                                  qol::contracts::QolItemHandle* checkpoint) {
    assert(g_virtual_bag_lock_held);
    ++g_callback_calls;
    return g_callback_registry->begin_invalidation(item, provider, checkpoint)
               ? qol::contracts::kQolItemHandleStatusOk
               : qol::contracts::kQolItemHandleStatusNotFound;
}

int32_t finish_invalidation_locked(qol::contracts::QolItemHandle checkpoint,
                                   int32_t committed) {
    assert(g_virtual_bag_lock_held);
    ++g_callback_calls;
    return g_callback_registry->finish_invalidation(checkpoint, committed != 0)
               ? qol::contracts::kQolItemHandleStatusOk
               : qol::contracts::kQolItemHandleStatusStale;
}

}  // namespace

namespace qol::game {
bool game_host_is_main_thread() { return g_main_thread; }
}

int main() {
    using qol::contracts::QolItemHandle;
    using qol::game::ItemHandleError;
    using qol::game::ItemHandleRegistry;

    ItemHandleRegistry table(&is_main_thread);
    int first_object = 1;
    int second_object = 2;
    constexpr uint32_t provider_a = 3;
    constexpr uint32_t provider_b = 4;
    ItemHandleError error{};
    const QolItemHandle first = table.register_item(&first_object, provider_a, &error);
    assert(first.slot != 0 && first.generation != 0 && error == ItemHandleError::kNone);
    assert(table.register_item(&first_object, provider_a).slot == first.slot);
    assert(table.resolve_item(first, provider_a).item == &first_object);
    assert(table.resolve_item(first, provider_b).error == ItemHandleError::kType);
    assert(!table.invalidate_item(&first_object, provider_b));
    assert(table.invalidate_item(&first_object, provider_a));
    assert(table.resolve_item(first, provider_a).error == ItemHandleError::kStale);

    const QolItemHandle reused = table.register_item(&second_object, provider_a);
    assert(reused.slot == first.slot && reused.generation != first.generation);
    assert(table.resolve_item(first, provider_a).error == ItemHandleError::kStale);

    g_main_thread = false;
    assert(table.register_item(&first_object, 0, &error).slot == 0);
    assert(error == ItemHandleError::kWrongThread);
    assert(table.resolve_item(reused).error == ItemHandleError::kWrongThread);
    g_main_thread = true;

    ItemHandleRegistry wrap_table;
    const QolItemHandle before_wrap = wrap_table.register_item(&first_object);
    assert(wrap_table.invalidate_item(before_wrap));
    wrap_table.set_generation_for_test(before_wrap.slot, UINT32_MAX);
    const QolItemHandle last_generation = wrap_table.register_item(&second_object);
    assert(last_generation.slot == before_wrap.slot && last_generation.generation == UINT32_MAX);
    assert(wrap_table.invalidate_item(last_generation));
    const QolItemHandle after_retire = wrap_table.register_item(&first_object);
    assert(after_retire.slot != before_wrap.slot && after_retire.generation == 1);

    ItemHandleRegistry lifecycle_table;
    const QolItemHandle merged_source = lifecycle_table.register_item(&first_object, provider_a);
    assert(lifecycle_table.invalidate_item(&first_object));
    assert(lifecycle_table.resolve_item(merged_source, provider_a).error == ItemHandleError::kStale);
    const QolItemHandle adopted = lifecycle_table.register_item(&first_object, provider_a);
    assert(lifecycle_table.rebind_item(&first_object, provider_b));
    assert(lifecycle_table.resolve_item(adopted, provider_b).item == &first_object);
    assert(lifecycle_table.resolve_item(adopted, provider_a).error == ItemHandleError::kType);
    assert(lifecycle_table.register_item(&second_object, provider_a).slot != 0);
    lifecycle_table.invalidate_all();
    assert(lifecycle_table.resolve_item(adopted, provider_b).error == ItemHandleError::kStale);
    const QolItemHandle after_reload = lifecycle_table.register_item(&first_object, provider_b);
    assert(after_reload.generation != adopted.generation);
    assert(lifecycle_table.invalidate_item(&first_object, provider_b));
    assert(lifecycle_table.resolve_item(after_reload, provider_b).error == ItemHandleError::kStale);
    const QolItemHandle partial_consume = lifecycle_table.register_item(&first_object, provider_a);
    assert(lifecycle_table.resolve_item(partial_consume, provider_a).item == &first_object);
    QolItemHandle removal_checkpoint{};
    assert(lifecycle_table.begin_invalidation(&first_object, provider_a, &removal_checkpoint));
    assert(lifecycle_table.resolve_item(partial_consume, provider_a).error == ItemHandleError::kStale);
    assert(lifecycle_table.finish_invalidation(removal_checkpoint, false));
    assert(lifecycle_table.resolve_item(partial_consume, provider_a).item == &first_object);

    assert(lifecycle_table.begin_lifecycle_transition());
    assert(lifecycle_table.resolve_item(partial_consume, provider_a).error == ItemHandleError::kStale);
    assert(lifecycle_table.register_item(&second_object, provider_a, &error).slot == 0);
    assert(error == ItemHandleError::kTransition);
    lifecycle_table.finish_lifecycle_transition(false);
    assert(lifecycle_table.resolve_item(partial_consume, provider_a).item == &first_object);
    assert(lifecycle_table.begin_lifecycle_transition());
    lifecycle_table.finish_lifecycle_transition(true);
    assert(lifecycle_table.resolve_item(partial_consume, provider_a).error == ItemHandleError::kStale);

    ItemHandleRegistry save_reload_table;
    const QolItemHandle native_slot_item = save_reload_table.register_item(
        &first_object, qol::contracts::kQolItemHandleProviderNative);
    const QolItemHandle provider_item = save_reload_table.register_item(
        &second_object, qol::contracts::kQolItemHandleProviderExtensionBag);
    save_reload_table.invalidate_provider(qol::contracts::kQolItemHandleProviderNative);
    assert(save_reload_table.resolve_item(
               native_slot_item, qol::contracts::kQolItemHandleProviderNative).error ==
           ItemHandleError::kStale);
    assert(save_reload_table.resolve_item(
               provider_item, qol::contracts::kQolItemHandleProviderExtensionBag).item ==
           &second_object);

    ItemHandleRegistry eb_registry;
    g_callback_registry = &eb_registry;
    const qol::contracts::QolItemHandleApi item_handle_api{
        &register_locked, &invalidate_locked, &rebind_locked,
        &begin_invalidation_locked, &finish_invalidation_locked};
    {
        const std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        g_virtual_bag_lock_held = true;
        qol::contracts::QolItemHandle materialized{};
        assert(item_handle_api.register_item(
                   &first_object, qol::contracts::kQolItemHandleProviderExtensionBag,
                   &materialized) == qol::contracts::kQolItemHandleStatusOk);
        assert(item_handle_api.rebind_item(
                   &first_object, qol::contracts::kQolItemHandleProviderNative) ==
               qol::contracts::kQolItemHandleStatusOk);
        const auto resolved = eb_registry.resolve_item(
            materialized, qol::contracts::kQolItemHandleProviderNative);
        assert(resolved.item == &first_object && resolved.error == ItemHandleError::kNone);
        qol::contracts::QolItemHandle checkpoint{};
        assert(item_handle_api.begin_invalidation(
                   &first_object, qol::contracts::kQolItemHandleProviderNative,
                   &checkpoint) == qol::contracts::kQolItemHandleStatusOk);
        assert(item_handle_api.finish_invalidation(
                   checkpoint, 0) == qol::contracts::kQolItemHandleStatusOk);
        g_virtual_bag_lock_held = false;
    }
    {
        const std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        g_virtual_bag_lock_held = true;
        assert(item_handle_api.invalidate_item(
                   &first_object, qol::contracts::kQolItemHandleProviderNative) ==
               qol::contracts::kQolItemHandleStatusOk);
        g_virtual_bag_lock_held = false;
    }
    assert(g_callback_calls == 5 && g_refresh_calls == 0);

    ItemHandleRegistry full_table;
    int objects[ItemHandleRegistry::kCapacity + 1]{};
    for (size_t i = 0; i < ItemHandleRegistry::kCapacity; ++i) {
        assert(full_table.register_item(&objects[i]).slot != 0);
    }
    assert(full_table.high_watermark() == ItemHandleRegistry::kCapacity);
    assert(full_table.register_item(&objects[ItemHandleRegistry::kCapacity], 0, &error).slot == 0);
    assert(error == ItemHandleError::kFull);
    assert(full_table.resolve_item({1, 1}).item == &objects[0]);
    return 0;
}
