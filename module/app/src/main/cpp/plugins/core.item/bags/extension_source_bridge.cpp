#include "extension_source_bridge.h"

#include "contracts/inventory_extension.h"

namespace qol::features::core_item::bags {
namespace {

constexpr int32_t kProbeBagCap = 128;
const contracts::QolInventoryExtensionProvider* g_provider = nullptr;

QolResult unsupported() {
    constexpr char kMessage[] = "extension bag write unsupported";
    return contracts::error(kQolErrUnsupported, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

QolResult provider_unavailable() {
    constexpr char kMessage[] = "extension bag provider unavailable";
    return contracts::error(kQolErrNotReady, kMessage,
                            static_cast<uint32_t>(sizeof(kMessage) - 1));
}

bool owns_bag(int32_t bag) {
    return g_provider != nullptr && g_provider->owns_bag != nullptr && bag >= 0 &&
           g_provider->owns_bag(bag) != 0;
}

int32_t count() {
    if (g_provider == nullptr || g_provider->owns_bag == nullptr) return 0;
    int32_t found = 0;
    while (found < kProbeBagCap && g_provider->owns_bag(found) != 0) ++found;
    return found;
}

QolResult info(int32_t bag, contracts::QolBagInfo* out) {
    if (out == nullptr || bag < 0) {
        constexpr char kMessage[] = "invalid extension bag";
        return contracts::error(kQolErrInvalidArgument, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if (!owns_bag(bag) || g_provider->bag_capacity == nullptr ||
        g_provider->item_ref_at == nullptr) {
        return provider_unavailable();
    }
    int32_t capacity = 0;
    QolResult result = g_provider->bag_capacity(bag, &capacity);
    if (!contracts::succeeded(result)) return result;
    if (capacity < 0) {
        constexpr char kMessage[] = "invalid extension bag capacity";
        return contracts::error(kQolErrInvalidArgument, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    int32_t slot_count = 0;
    for (int32_t slot = 0; slot < capacity; ++slot) {
        contracts::QolInventoryExtensionItemRef item{};
        result = g_provider->item_ref_at(bag, slot, &item);
        if (!contracts::succeeded(result)) {
            if (result.code == kQolErrNotFound) continue;
            return result;
        }
        if (item.item.slot != 0 && item.item.generation != 0) ++slot_count;
    }
    *out = contracts::QolBagInfo{capacity, slot_count};
    return contracts::ok();
}

QolResult item_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    if (out == nullptr || bag < 0 || slot < 0) {
        constexpr char kMessage[] = "invalid extension bag slot";
        return contracts::error(kQolErrInvalidArgument, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if (!owns_bag(bag) || g_provider->item_ref_at == nullptr) return provider_unavailable();
    contracts::QolInventoryExtensionItemRef item{};
    const QolResult result = g_provider->item_ref_at(bag, slot, &item);
    if (!contracts::succeeded(result)) return result;
    out->kind = item.kind;
    out->bag = item.bag;
    out->slot = item.slot;
    out->category = item.category;
    out->count = item.count;
    out->item = item.item;
    out->space_id = contracts::kQolBagSpaceFirstRegistered;
    return contracts::ok();
}

struct ForEachContext {
    contracts::QolInventoryBagSlotFn callback;
    void* context;
};

int32_t visit_provider_item(const contracts::QolInventoryExtensionItemRef* item, void* raw) {
    if (item == nullptr || raw == nullptr) return 0;
    auto* context = static_cast<ForEachContext*>(raw);
    contracts::QolInventoryItemRef ref{};
    ref.kind = item->kind;
    ref.bag = item->bag;
    ref.slot = item->slot;
    ref.category = item->category;
    ref.count = item->count;
    ref.item = item->item;
    ref.space_id = contracts::kQolBagSpaceFirstRegistered;
    return context->callback(&ref, context->context);
}

QolResult for_each_item(contracts::QolInventoryBagSlotFn fn, void* ctx) {
    if (fn == nullptr) {
        constexpr char kMessage[] = "invalid extension item callback";
        return contracts::error(kQolErrInvalidArgument, kMessage,
                                static_cast<uint32_t>(sizeof(kMessage) - 1));
    }
    if (g_provider == nullptr || g_provider->for_each_item == nullptr) {
        return provider_unavailable();
    }
    ForEachContext context{fn, ctx};
    return g_provider->for_each_item(&visit_provider_item, &context);
}

QolResult insert(const contracts::QolBagInsertRequest*, contracts::QolBagMutationResult*) {
    return unsupported();
}

QolResult remove(contracts::QolItemHandle, int32_t,
                 contracts::QolBagMutationResult*) {
    return unsupported();
}

QolResult identify(contracts::QolItemHandle, contracts::QolBagRef*) {
    return unsupported();
}

QolResult use(contracts::QolBagRef, int32_t) {
    return unsupported();
}

const contracts::QolBagSource kSource{
    contracts::kBagSourceVersion,
    contracts::kQolBagSpaceFirstRegistered,
    &count,
    &info,
    &item_at,
    &for_each_item,
    &insert,
    &remove,
    &identify,
    &use,
};

}  // namespace

void ExtensionSourceBridge::bind(const contracts::QolInventoryExtensionProvider* provider) {
    g_provider = provider;
}

const contracts::QolBagSource* ExtensionSourceBridge::source() {
    return &kSource;
}

}  // namespace qol::features::core_item::bags
