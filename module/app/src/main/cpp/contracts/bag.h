#pragma once

// 袋源注册协议：core.item 提供注册面，其他插件按空间注册自己的袋数据源与写入原语。
// 设计见 docs/reference/plugins/core.item.md（QolBagSource 契约）。
//
// 约束：
// - 注册/注销只在主线程、插件 activate/deactivate 时调用。
// - 同一 space_id 重复注册失败，不覆盖既有源。
// - 回调一律主线程调用；不得跨线程持有返回的句柄以外的原生指针。

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "game_ports/item_port.h"
#include "item_handle.h"
#include "result.h"
#include "version.h"

namespace qol::contracts {

constexpr const char* kBagSourceCapabilityId = "inventory.bag_source";
// 1.3：尾部追加 replace_item（原子替换 provider 槽内物品）。
constexpr QolVersion kBagSourceVersion{1, 3};

// 跨介质物品转移记录：payload 格式由来源源定义，core.item 只透传。
// 有效长度 <= kQolBagTransferPayloadMaxSize；缓冲区固定 256 字节，禁止动态分配。
constexpr uint32_t kQolBagTransferPayloadCapacity = 256;
constexpr uint32_t kQolBagTransferPayloadMaxSize = 255;

// INVEN_FindSaveSlot/SaveItemDirect 使用的扁平袋号：注册源空间 >= 2。
// 原生袋号 0..18 与之不相交；低层函数只接受 int32 袋号，靠该区间区分来源。
constexpr int32_t kQolBagFlatRegisteredBase = 0x4000;
constexpr int32_t kQolBagFlatStride = 64;

inline int32_t qol_bag_flat_id(int32_t space_id, int32_t bag) {
    return kQolBagFlatRegisteredBase + space_id * kQolBagFlatStride + bag;
}

inline bool qol_bag_flat_decode(int32_t flat_bag, int32_t* out_space_id, int32_t* out_bag) {
    if (out_space_id == nullptr || out_bag == nullptr || flat_bag < kQolBagFlatRegisteredBase) {
        return false;
    }
    const int32_t offset = flat_bag - kQolBagFlatRegisteredBase;
    const int32_t space_id = offset / kQolBagFlatStride;
    const int32_t bag = offset % kQolBagFlatStride;
    if (space_id < kQolBagSpaceFirstRegistered || bag < 0) return false;
    *out_space_id = space_id;
    *out_bag = bag;
    return true;
}

struct QolBagTransfer {
    int32_t category;
    int32_t count;
    uint32_t payload_size;
    uint8_t payload[kQolBagTransferPayloadCapacity];
};

struct QolBagMutationResult {
    QolBagRef ref;
    QolItemHandle item;
    int32_t count;
};

struct QolBagSource {
    QolVersion version;
    int32_t space_id;

    // 读面。
    int32_t (*count)();
    QolResult (*info)(int32_t bag, QolBagInfo* out);
    QolResult (*item_at)(int32_t bag, int32_t slot, QolInventoryItemRef* out);
    QolResult (*for_each_item)(QolInventoryBagSlotFn fn, void* ctx);

    // 转移面：导出（只读） → 目标导入 → 源移除。三步均由 core.item 编排。
    QolResult (*export_item)(QolItemHandle item, int32_t count, QolBagTransfer* out);
    QolResult (*import_item)(const QolBagTransfer* transfer, int32_t preferred_bag,
                             int32_t preferred_slot, QolBagMutationResult* out);
    QolResult (*remove)(QolItemHandle expected, int32_t count, QolBagMutationResult* out);

    // 可选面：定位与使用。缺失时对应功能返回 kQolErrUnsupported。
    QolResult (*identify)(QolItemHandle item, QolBagRef* out, int32_t* out_slot);
    QolResult (*use)(QolBagRef ref, int32_t slot);

    // 提交 provider 权威副本：item.payload.update 已把字段写到既有物化对象上，
    // 来源只更新 descriptor/category/count/hash 并持久化/刷新。expected 必须仍是
    // 当前槽物品；成功返回同一 handle（对象身份保留），失败 descriptor 不变。
    QolResult (*replace_item)(QolItemHandle expected, const QolBagTransfer* replacement,
                              QolBagMutationResult* out);
};

struct QolBagSourceRegistryApi {
    uint32_t struct_size;
    QolResult (*register_source)(const QolBagSource* source);
    QolResult (*unregister_source)(int32_t space_id);
    uint32_t (*source_count)();
    QolResult (*source_at)(uint32_t index, const QolBagSource** out_source);
};

inline bool qol_bag_source_registry_api_valid(const QolBagSourceRegistryApi* api) {
    constexpr uint32_t kRequiredSize =
        static_cast<uint32_t>(offsetof(QolBagSourceRegistryApi, source_at) +
                              sizeof(decltype(QolBagSourceRegistryApi::source_at)));
    return api != nullptr && api->struct_size >= kRequiredSize &&
           api->register_source != nullptr && api->unregister_source != nullptr;
}

inline bool qol_bag_source_valid(const QolBagSource* source) {
    return source != nullptr && source->version.major == kBagSourceVersion.major &&
           source->version.minor >= kBagSourceVersion.minor &&
           source->space_id >= kQolBagSpaceFirstRegistered && source->count != nullptr &&
           source->info != nullptr && source->item_at != nullptr &&
           source->for_each_item != nullptr && source->export_item != nullptr &&
           source->import_item != nullptr && source->remove != nullptr &&
           source->replace_item != nullptr;
}

inline bool qol_bag_ref_is_task_bag(const QolBagRef& ref) {
    return ref.space_id == kQolBagSpaceOriginal && ref.bag == kQolTaskBagIndex;
}

static_assert(std::is_standard_layout<QolBagTransfer>::value,
              "QolBagTransfer must remain standard layout");
static_assert(std::is_trivially_copyable<QolBagTransfer>::value,
              "QolBagTransfer must remain trivially copyable");
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
