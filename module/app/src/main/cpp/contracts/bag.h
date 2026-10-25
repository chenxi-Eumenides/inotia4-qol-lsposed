#pragma once

// 袋源注册协议：core.item 提供注册面，其他插件按空间注册自己的袋数据源与写入原语。
// 设计见 docs/refactor/core-item/architecture.md §4.3。
//
// 约束：
// - 注册/注销只在主线程、插件 activate/deactivate 时调用。
// - 同一 space_id 重复注册失败，不覆盖既有源。
// - 回调一律主线程调用；不得跨线程持有返回的句柄以外的原生指针。

#include <cstdint>
#include <type_traits>

#include "game_ports/item_port.h"
#include "item_handle.h"
#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kBagSourceCapabilityId = "inventory.bag_source";
constexpr QolVersion kBagSourceVersion{1, 0};

struct QolBagInsertRequest {
    int32_t category;
    int32_t count;
    // -1 表示由源自行选择目标袋/槽。
    int32_t preferred_bag;
    int32_t preferred_slot;
    int32_t socket_total;
    int32_t enhance_remaining;
    int32_t rarity_group;
};

struct QolBagMutationResult {
    QolBagRef ref;
    QolItemHandle item;
    int32_t count;
};

struct QolBagSource {
    QolVersion version;
    int32_t space_id;
    int32_t (*count)();
    QolResult (*info)(int32_t bag, QolBagInfo* out);
    QolResult (*item_at)(int32_t bag, int32_t slot, QolInventoryItemRef* out);
    QolResult (*for_each_item)(QolInventoryBagSlotFn fn, void* ctx);
    QolResult (*insert)(const QolBagInsertRequest* request, QolBagMutationResult* out);
    QolResult (*remove)(QolItemHandle expected, int32_t count, QolBagMutationResult* out);
    QolResult (*identify)(QolItemHandle item, QolBagRef* out);
    QolResult (*use)(QolBagRef ref, int32_t slot);
};

struct QolBagSourceRegistryApi {
    uint32_t struct_size;
    QolResult (*register_source)(const QolBagSource* source);
    QolResult (*unregister_source)(int32_t space_id);
};

inline bool qol_bag_source_valid(const QolBagSource* source) {
    return source != nullptr && source->version.major == kBagSourceVersion.major &&
           source->version.minor <= kBagSourceVersion.minor && source->space_id >=
               kQolBagSpaceFirstRegistered &&
           source->count != nullptr && source->info != nullptr && source->item_at != nullptr &&
           source->for_each_item != nullptr;
}

inline bool qol_bag_ref_is_task_bag(const QolBagRef& ref) {
    return ref.space_id == kQolBagSpaceOriginal && ref.bag == kQolTaskBagIndex;
}

static_assert(std::is_standard_layout<QolBagRef>::value, "QolBagRef must remain standard layout");
static_assert(std::is_trivially_copyable<QolBagRef>::value,
              "QolBagRef must remain trivially copyable");
static_assert(std::is_standard_layout<QolBagInfo>::value,
              "QolBagInfo must remain standard layout");
static_assert(std::is_trivially_copyable<QolBagInfo>::value,
              "QolBagInfo must remain trivially copyable");
static_assert(std::is_standard_layout<QolBagSource>::value,
              "QolBagSource must remain standard layout");
static_assert(std::is_trivially_copyable<QolBagSource>::value,
              "QolBagSource must remain trivially copyable");
static_assert(std::is_standard_layout<QolBagSourceRegistryApi>::value,
              "QolBagSourceRegistryApi must remain standard layout");
static_assert(std::is_trivially_copyable<QolBagSourceRegistryApi>::value,
              "QolBagSourceRegistryApi must remain trivially copyable");

}  // namespace qol::contracts
