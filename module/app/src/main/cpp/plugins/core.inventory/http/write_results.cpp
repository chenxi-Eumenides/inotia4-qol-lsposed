#include "write_results.h"
#include "plugins/core.inventory/api/access.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>

#include "plugins/core.inventory/api/bag_source.h"
#include "plugins/core.inventory/http/json_lite.h"
#include "plugins/core.inventory/http/response.h"
#include "plugins/core.inventory/http/write_handler.h"
#include "plugins/core.inventory/runtime/context.h"
#include "plugins/core.inventory/transfer/move_item.h"
#include "plugins/core.inventory/transfer/native_transport.h"
#include "plugins/core.inventory/transfer/source_transport.h"
#include "plugins/core.inventory/usecases/item_helpers.h"
#include "plugins/core.inventory/usecases/read_inventory.h"

namespace qol::features::core_inventory {
namespace {

constexpr char kSetMoneySuccess[] = "{\"ok\":true}";
constexpr char kSetMoneyNotInGame[] = "{\"ok\":false,\"error\":\"not in game\"}";
constexpr char kSetMoneySymbolMissing[] = "{\"ok\":false,\"error\":\"symbol not resolved\"}";
constexpr char kSetMoneyFailed[] = "{\"ok\":false,\"error\":\"set money failed\"}";
constexpr char kInventoryUnavailable[] =
    "{\"ok\":false,\"error\":\"inventory state unavailable\"}";
constexpr char kInvalidBag[] = "{\"ok\":false,\"error\":\"invalid inventory bag\"}";

struct ResultBagRef {
    int32_t space_id;
    int32_t bag;
    int32_t slot;
};

struct SlotKey {
    int32_t space_id;
    int32_t bag;
    int32_t slot;

    bool operator<(const SlotKey& other) const {
        return std::tie(space_id, bag, slot) < std::tie(other.space_id, other.bag, other.slot);
    }
};

using InventoryResultSnapshot = std::map<SlotKey, std::string>;
using ForcedSlots = std::set<SlotKey>;

// 结果处理器在主线程执行，注册源 count 可直接读源回调（与迁移前 core.item 一致）。
bool result_space_count(int32_t space_id, int32_t* out_count) {
    if (out_count == nullptr) return false;
    if (space_id >= contracts::kQolBagSpaceFirstRegistered) {
        const contracts::QolBagSource* source = find_bag_source(space_id);
        if (source == nullptr || source->count == nullptr) {
            *out_count = 0;
            return true;
        }
        const int32_t count = source->count();
        if (count < 0) return false;
        *out_count = count;
        return true;
    }
    if (out_count == nullptr) {
        return false;
    }
    int32_t count = 0;
    const QolResult result = inventory_access_api().native_bag_count(space_id, &count);
    if (!contracts::succeeded(result)) {
        if (space_id != contracts::kQolBagSpaceOriginal &&
            (result.code == kQolErrUnsupported || result.code == kQolErrNotReady)) {
            *out_count = 0;
            return true;
        }
        return false;
    }
    if (count < 0) return false;
    *out_count = count;
    return true;
}

bool decode_write_bag(int32_t host_bag, ResultBagRef* out) {
    if (host_bag < 0 || out == nullptr) return false;
    int32_t original_count = 0;
    int32_t c18_count = 0;
    if (!result_space_count(contracts::kQolBagSpaceOriginal, &original_count) ||
        !result_space_count(contracts::kQolBagSpaceVariant, &c18_count)) {
        return false;
    }
    if (host_bag < original_count) {
        *out = {contracts::kQolBagSpaceOriginal, host_bag, -1};
        return true;
    }
    if (host_bag < contracts::kQolOriginalBagCount) return false;
    int32_t remaining = host_bag - original_count;
    if (remaining < c18_count) {
        *out = {contracts::kQolBagSpaceVariant, remaining, -1};
        return true;
    }
    remaining -= c18_count;
    for (uint32_t index = 0; index < registered_bag_source_count(); ++index) {
        const contracts::QolBagSource* source = registered_bag_source_at(index);
        if (source == nullptr) continue;
        int32_t count = 0;
        if (!result_space_count(source->space_id, &count)) return false;
        if (remaining < count) {
            *out = {source->space_id, remaining, -1};
            return true;
        }
        remaining -= count;
    }
    return false;
}

bool write_slot_key(int32_t host_bag, int32_t slot, SlotKey* out) {
    if (out == nullptr || slot < 0) return false;
    ResultBagRef ref{};
    if (!decode_write_bag(host_bag, &ref)) return false;
    *out = {ref.space_id, ref.bag, slot};
    return true;
}

// 结果读取沿用迁移前的容错口径：单槽 NotFound/NotReady/Unsupported 渲染为 null，
// 不因某个袋源瞬时不可读而让已经提交的 move 丢失结果体。
QolResult result_bag_info(const contracts::QolBagRef& ref, contracts::QolBagInfo* out) {
    if (ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
        const contracts::QolBagSource* source = find_bag_source(ref.space_id);
        if (source == nullptr || source->info == nullptr) {
            return contracts::error(kQolErrUnsupported, "registered bag source unavailable", 33);
        }
        return source->info(ref.bag, out);
    }
    return inventory_access_api().native_bag_info(&ref, out);
}

QolResult result_item_at(const contracts::QolBagRef& ref, int32_t slot,
                          contracts::QolInventoryItemRef* out) {
    if (ref.space_id >= contracts::kQolBagSpaceFirstRegistered) {
        const contracts::QolBagSource* source = find_bag_source(ref.space_id);
        if (source == nullptr || source->item_at == nullptr) {
            return contracts::error(kQolErrUnsupported, "registered bag source unavailable", 33);
        }
        return source->item_at(ref.bag, slot, out);
    }
    return inventory_access_api().native_item_at(&ref, slot, out);
}

bool read_enriched_item(const contracts::QolBagRef& bag, int32_t slot, std::string* out) {
    if (out == nullptr || g_port == nullptr || g_port->read.inventory_item_view == nullptr) {
        return false;
    }
    contracts::QolInventoryItemRef item_ref{};
    const QolResult ref_result = result_item_at(bag, slot, &item_ref);
    if (!contracts::succeeded(ref_result)) {
        if (ref_result.code == kQolErrNotFound || ref_result.code == kQolErrNotReady ||
            ref_result.code == kQolErrUnsupported) {
            *out = "null";
            return true;
        }
        return false;
    }
    if (item_ref.space_id != bag.space_id || item_ref.bag != bag.bag || item_ref.slot != slot) {
        return false;
    }
    const contracts::QolItemHandle item_handle = item_ref.item;
    if (item_handle.slot == 0 || item_handle.generation == 0) {
        *out = "null";
        return true;
    }
    contracts::QolItemInventoryView view{};
    if (!contracts::succeeded(g_port->read.inventory_item_view(item_handle, &view)) ||
        !inventory_option_counts_valid(view)) {
        return false;
    }
    uint32_t count = 0;
    bool count_present = false;
    if (!effective_item_count(item_handle, view.count_supported != 0, &count, &count_present)) {
        return false;
    }
    std::string raw;
    append_inventory_item(&raw, slot, view, count_present, count);
    JsonValue item{};
    if (!json_parse(raw.data(), static_cast<uint32_t>(raw.size()), &item) ||
        !enrich_item_value(item, g_port, out)) {
        return false;
    }
    return true;
}

bool capture_inventory_result(InventoryResultSnapshot* out) {
    if (out == nullptr ||
        !contracts::qol_inventory_access_api_valid(&inventory_access_api())) {
        return false;
    }
    InventoryResultSnapshot snapshot;
    const uint32_t space_count = 2 + registered_bag_source_count();
    for (uint32_t space_index = 0; space_index < space_count; ++space_index) {
        const contracts::QolBagSource* registered_source =
            space_index < 2 ? nullptr : registered_bag_source_at(space_index - 2);
        const int32_t space_id = space_index == 0
                                     ? contracts::kQolBagSpaceOriginal
                                     : space_index == 1
                                           ? contracts::kQolBagSpaceVariant
                                           : registered_source != nullptr
                                                 ? registered_source->space_id
                                                 : -1;
        if (space_id < 0) continue;
        int32_t bag_count = 0;
        if (!result_space_count(space_id, &bag_count)) return false;
        for (int32_t bag = 0; bag < bag_count; ++bag) {
            const contracts::QolBagRef ref{space_id, bag};
            contracts::QolBagInfo info{};
            if (!contracts::succeeded(result_bag_info(ref, &info)) || info.capacity < 0) {
                return false;
            }
            for (int32_t slot = 0; slot < info.capacity; ++slot) {
                std::string item_json;
                if (!read_enriched_item(ref, slot, &item_json)) return false;
                snapshot.emplace(SlotKey{space_id, bag, slot}, std::move(item_json));
            }
        }
    }
    *out = std::move(snapshot);
    return true;
}

const std::string& result_item_at(const InventoryResultSnapshot& snapshot, const SlotKey& key) {
    static const std::string kNull = "null";
    const auto found = snapshot.find(key);
    return found == snapshot.end() ? kNull : found->second;
}

void append_result_slot(std::string& body, const SlotKey& key,
                        const InventoryResultSnapshot& snapshot, bool* first) {
    if (!*first) body.push_back(',');
    *first = false;
    body += "{\"space\":" + std::to_string(key.space_id) + ",\"bag\":" +
            std::to_string(key.bag) + ",\"slot\":" + std::to_string(key.slot) +
            ",\"item\":" + result_item_at(snapshot, key) + "}";
}

void append_unforced_inventory_changes(
    std::string& body, const InventoryResultSnapshot& before,
    const InventoryResultSnapshot& after, const ForcedSlots& forced, bool* first) {
    std::set<SlotKey> keys;
    for (const auto& entry : before) keys.insert(entry.first);
    for (const auto& entry : after) keys.insert(entry.first);
    for (const SlotKey& key : keys) {
        if (forced.count(key) != 0 || result_item_at(before, key) == result_item_at(after, key)) {
            continue;
        }
        append_result_slot(body, key, after, first);
    }
}

bool copy_result_truncated(char* out, uint32_t capacity, uint32_t* out_size,
                           const std::string& body) {
    if (out == nullptr || out_size == nullptr) return false;
    const uint32_t copied = static_cast<uint32_t>(std::min<std::size_t>(body.size(), capacity));
    if (copied != 0) std::memcpy(out, body.data(), copied);
    *out_size = static_cast<uint32_t>(body.size());
    return true;
}

bool copy_result_text(char* out, uint32_t capacity, uint32_t* out_size, const char* body,
                      uint32_t body_size) {
    if (out == nullptr || out_size == nullptr) return false;
    const uint32_t copied = std::min(body_size, capacity);
    if (copied != 0) std::memcpy(out, body, copied);
    *out_size = body_size;
    return true;
}

bool write_operation_error(const QolResult& operation, const char* fallback, char* out,
                           uint32_t capacity, uint32_t* out_size) {
    const char* message = operation.message != nullptr ? operation.message : fallback;
    const uint32_t message_size = operation.message != nullptr
                                      ? operation.message_len
                                      : static_cast<uint32_t>(std::strlen(message));
    const std::string body = std::string("{\"ok\":false,\"error\":\"") +
                             std::string(message, message_size) + "\"}";
    (void)copy_result_truncated(out, capacity, out_size, body);
    return false;
}

bool is_result_native_space(int32_t space_id) {
    return space_id == contracts::kQolBagSpaceOriginal ||
           space_id == contracts::kQolBagSpaceVariant;
}

bool write_inventory_move_result(const InventoryWritePayload& command, char* out,
                                 uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    const contracts::QolItemWriteArgs& args = command.args;
    ResultBagRef from{};
    ResultBagRef to{};
    if (out == nullptr || out_size == nullptr || args.i1 < 0 || args.i2 <= 0 ||
        command.from_space < 0 || command.to_space < 0 || !decode_write_bag(args.i0, &from) ||
        !decode_write_bag(args.i3, &to) || from.space_id != command.from_space ||
        to.space_id != command.to_space) {
        return copy_result_text(out, capacity, out_size, kInvalidBag,
                                static_cast<uint32_t>(sizeof(kInvalidBag) - 1));
    }
    // 原生同空间（space 0/1）走宿主 move 端口；其余（原生跨空间、注册源相关）
    // 走 inventory.transfer 编排（导出 → 导入 → 源移除，内部补偿）。
    const bool native_same_space = is_result_native_space(from.space_id) && from.space_id == to.space_id;

    InventoryResultSnapshot before{};
    // before 快照失败不再静默：记录可用性；失败时仍执行 move，但差异段无基准可算，
    // 只输出 forced 槽，避免把全部 after 槽误报成变化。
    const bool before_available = capture_inventory_result(&before);

    contracts::QolItemInventoryStateWriteResult operation_data{};
    contracts::QolBagMutationResult placed{};
    QolResult operation = contracts::ok();
    if (native_same_space) {
        if (g_item_state_write == nullptr || g_item_state_write->move_item == nullptr) {
            operation = contracts::error(kQolErrUnsupported, "item operation unavailable", 26);
        } else {
            operation = g_item_state_write->move_item(args.i0, args.i1, args.i2, args.i3,
                                                      args.i4, &operation_data);
        }
    } else {
        const contracts::QolBagRef from_ref{from.space_id, from.bag};
        const contracts::QolBagRef to_ref{to.space_id, to.bag};
        operation =
            transfer::move_item::move_item(from_ref, args.i1, args.i2, to_ref, args.i4, &placed);
    }
    if (!contracts::succeeded(operation)) {
        return write_operation_error(operation,
                                     native_same_space ? "item operation failed"
                                                       : "item transfer failed",
                                     out, capacity, out_size);
    }
    // transfer 分支内部已刷新；原生同空间由 inventory HTTP 侧补齐发布面刷新。
    if (native_same_space) request_inventory_refresh();

    InventoryResultSnapshot after{};
    if (!capture_inventory_result(&after)) {
        // 与迁移前一致：宿主端口路径回 inventory state unavailable，transfer 路径静默失败。
        if (native_same_space) {
            return copy_result_text(out, capacity, out_size, kInventoryUnavailable,
                                    static_cast<uint32_t>(sizeof(kInventoryUnavailable) - 1));
        }
        return false;
    }

    ForcedSlots forced;
    SlotKey source_key{};
    const bool has_source = write_slot_key(args.i0, args.i1, &source_key);
    if (has_source) forced.insert(source_key);
    SlotKey target_key{};
    bool has_target = false;
    if (native_same_space) {
        has_target = write_slot_key(args.i3, args.i4, &target_key);
    } else {
        int32_t target_slot = args.i4;
        ResultBagRef target_ref = to;
        if (placed.item.slot != 0 && placed.item.generation != 0) {
            contracts::QolBagRef located{};
            int32_t located_slot = -1;
            const QolResult located_result =
                is_result_native_space(placed.ref.space_id)
                    ? transfer::native_transport::locate(placed.ref.space_id, placed.item,
                                                         &located, &located_slot)
                    : transfer::source_transport::locate(placed.ref.space_id, placed.item,
                                                         &located, &located_slot);
            if (contracts::succeeded(located_result) && located_slot >= 0) {
                target_ref = {located.space_id, located.bag, -1};
                target_slot = located_slot;
            }
        }
        if (target_slot >= 0) {
            target_key = {target_ref.space_id, target_ref.bag, target_slot};
            has_target = true;
        }
    }
    if (has_target) forced.insert(target_key);

    std::string body = "{\"ok\":true,\"slots\":[";
    bool first = true;
    if (has_source) append_result_slot(body, source_key, after, &first);
    if (has_target && (!has_source || source_key.space_id != target_key.space_id ||
                       source_key.bag != target_key.bag || source_key.slot != target_key.slot)) {
        append_result_slot(body, target_key, after, &first);
    }
    if (before_available) {
        append_unforced_inventory_changes(body, before, after, forced, &first);
    }
    body += "]}";
    return copy_result_truncated(out, capacity, out_size, body);
}

bool write_set_money_result(const contracts::QolItemWriteArgs& args, char* out, uint32_t capacity,
                            uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (g_item_money_write == nullptr || g_item_money_write->set_money == nullptr) {
        (void)copy_result_text(out, capacity, out_size, kSetMoneyFailed,
                               static_cast<uint32_t>(sizeof(kSetMoneyFailed) - 1));
        return false;
    }
    const QolResult operation = g_item_money_write->set_money(args.i64);
    const char* body = kSetMoneySuccess;
    uint32_t body_size = sizeof(kSetMoneySuccess) - 1;
    if (!contracts::succeeded(operation)) {
        if (operation.code == kQolErrNotReady) {
            body = kSetMoneyNotInGame;
            body_size = sizeof(kSetMoneyNotInGame) - 1;
        } else if (operation.code == kQolErrUnsupported) {
            body = kSetMoneySymbolMissing;
            body_size = sizeof(kSetMoneySymbolMissing) - 1;
        } else {
            body = kSetMoneyFailed;
            body_size = sizeof(kSetMoneyFailed) - 1;
        }
    }
    (void)copy_result_text(out, capacity, out_size, body, body_size);
    return contracts::succeeded(operation);
}

}  // namespace

bool inventory_write_result(void*, const uint8_t* payload, uint32_t size, char* out,
                            uint32_t capacity, uint32_t* out_size) {
    if (out_size != nullptr) *out_size = 0;
    if (payload == nullptr || out == nullptr || out_size == nullptr ||
        size != sizeof(InventoryWritePayload)) {
        return false;
    }
    InventoryWritePayload command{};
    std::memcpy(&command, payload, size);
    const auto kind = static_cast<contracts::QolItemWriteKind>(command.args.kind);
    if (kind == contracts::QolItemWriteKind::kMoveItem) {
        return write_inventory_move_result(command, out, capacity, out_size);
    }
    if (kind == contracts::QolItemWriteKind::kSetMoney) {
        return write_set_money_result(command.args, out, capacity, out_size);
    }
    return false;
}
}  // namespace qol::features::core_inventory
