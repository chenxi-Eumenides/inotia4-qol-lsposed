#include "game/adapter_ports/item_port.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "contracts/game_ports/ports.h"
#include "contracts/item_facts.h"
#include "game/access/attr_range_ui.h"
#include "game/access/game_access.h"
#include "game/access/inventory.h"
#include "game/access/item_class.h"
#include "game/access/item_handle_registry.h"
#include "game/access/item_sell_quote.h"
#include "game/adapter_ports/inventory_extension_facade.h"
#include "game/adapter_ports/item_count_facade.h"
#include "game/access/shop.h"
#include "game/access/world.h"
#include "game/read/item_class.h"
#include "game/state/game_state.h"
#include "game/variants/game_variant.h"

// JSON 构造器仍供未迁移的 game-system write/cache 路径使用；core.item owner 不用此入口。
std::string build_inventory_json();

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, "not ready"); }

QolResult resolve_item_handle(contracts::QolItemHandle handle, void** out_item) {
    if (out_item == nullptr || handle.slot == 0 || handle.generation == 0) {
        return error_result(kQolErrInvalidArgument, "invalid item handle");
    }
    const ItemHandleResolution resolution =
        item_handle_registry().resolve_item(handle, contracts::kQolItemHandleOwnerAny);
    if (resolution.error == ItemHandleError::kWrongThread) {
        return error_result(kQolErrBlocked, "wrong thread");
    }
    if (resolution.error == ItemHandleError::kType) {
        return error_result(kQolErrInvalidArgument, "item handle type mismatch");
    }
    if (resolution.error != ItemHandleError::kNone || resolution.item == nullptr) {
        return error_result(kQolErrNotFound, "stale item handle");
    }
    *out_item = resolution.item;
    return contracts::ok();
}

QolResult read_category(contracts::QolItemHandle handle, int32_t* out) {
    void* item = nullptr;
    const QolResult resolution = resolve_item_handle(handle, &item);
    if (!contracts::succeeded(resolution)) return resolution;
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    const int value = attr_item_category(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_real_equip(contracts::QolItemHandle handle, int32_t* out) {
    void* item = nullptr;
    const QolResult resolution = resolve_item_handle(handle, &item);
    if (!contracts::succeeded(resolution)) return resolution;
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (!item_equip_primitives_ready()) return not_ready();
    *out = item_is_real_equip(item) ? 1 : 0;
    return contracts::ok();
}

QolResult read_jewel_category(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    *out = attr_is_jewel_category(category) ? 1 : 0;
    return contracts::ok();
}

QolResult read_no_sell(int32_t category, int32_t* out) {
    if (category < 0 || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (fn_item_is_no_sell == nullptr) return not_ready();
    *out = fn_item_is_no_sell(category) != 0 ? 1 : 0;
    return contracts::ok();
}

QolResult read_sell_price(contracts::QolItemHandle handle, int64_t* out) {
    void* item = nullptr;
    const QolResult resolution = resolve_item_handle(handle, &item);
    if (!contracts::succeeded(resolution)) return resolution;
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    const int value = item_sell_price(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_quote(contracts::QolItemHandle handle, uint32_t sale_path,
                     contracts::QolSaleQuote* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid quote output");
    *out = {};
    void* item = nullptr;
    const QolResult resolution = resolve_item_handle(handle, &item);
    if (!contracts::succeeded(resolution)) return resolution;
    if (!item_sell_quote::quote(item, sale_path, qol::game_variant().series, out)) {
        return error_result(kQolErrNotReady, "item quote unavailable");
    }
    return contracts::ok();
}

QolResult read_cumulate_count(contracts::QolItemHandle handle, int32_t* out) {
    void* item = nullptr;
    const QolResult resolution = resolve_item_handle(handle, &item);
    if (!contracts::succeeded(resolution)) return resolution;
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    if (item_count_active()) {
        const contracts::QolItemCountApi* api = item_count_api_if_resident();
        if (api == nullptr || api->canonical == nullptr) return not_ready();
        uint32_t canonical = 0;
        const QolResult result = api->canonical(item, &canonical);
        if (!contracts::succeeded(result)) return result;
        *out = static_cast<int32_t>(canonical);
        return contracts::ok();
    }
    if (fn_get_cumulate_count == nullptr) return not_ready();
    *out = fn_get_cumulate_count(const_cast<void*>(item));
    return contracts::ok();
}

QolResult read_effective_cumulate_count(contracts::QolItemHandle handle, int32_t* out) {
    void* item = nullptr;
    const QolResult resolution = resolve_item_handle(handle, &item);
    if (!contracts::succeeded(resolution)) return resolution;
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    const contracts::QolItemCountApi* api = item_count_api_if_resident();
    if (api == nullptr || api->effective == nullptr) return not_ready();
    uint32_t effective = 0;
    const QolResult result = api->effective(item, &effective);
    if (!contracts::succeeded(result)) return result;
    *out = static_cast<int32_t>(effective);
    return contracts::ok();
}

QolResult read_count_limit(contracts::QolItemHandle handle, uint32_t* out_limit) {
    void* item = nullptr;
    const QolResult resolution = resolve_item_handle(handle, &item);
    if (!contracts::succeeded(resolution)) return resolution;
    if (item == nullptr || out_limit == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid count limit args");
    }
    const contracts::QolItemCountApi* api = item_count_api_if_resident();
    if (api == nullptr || api->is_encoded == nullptr || api->view_limit == nullptr) {
        return not_ready();
    }
    contracts::QolItemCountEncoding encoding = contracts::QolItemCountEncoding::kUnknown;
    const QolResult encoding_result = api->is_encoded(item, &encoding);
    if (!contracts::succeeded(encoding_result)) return encoding_result;
    if (encoding == contracts::QolItemCountEncoding::kUnknown) {
        return error_result(kQolErrUnsupported, "item count mode unavailable");
    }
    uint32_t limit = 0;
    const bool extended = encoding == contracts::QolItemCountEncoding::kEncoded && item_count_active();
    const QolResult limit_result = api->view_limit(extended ? 1 : 0, &limit);
    if (!contracts::succeeded(limit_result)) return limit_result;
    *out_limit = limit;
    return contracts::ok();
}

QolResult read_count_encoding(contracts::QolItemHandle handle,
                              contracts::QolItemCountEncoding* out_encoding) {
    void* item = nullptr;
    const QolResult resolution = resolve_item_handle(handle, &item);
    if (!contracts::succeeded(resolution)) return resolution;
    if (item == nullptr || out_encoding == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid count encoding args");
    }
    const contracts::QolItemCountApi* api = item_count_api_if_resident();
    if (api == nullptr || api->is_encoded == nullptr) return not_ready();
    contracts::QolItemCountEncoding encoding = contracts::QolItemCountEncoding::kUnknown;
    const QolResult result = api->is_encoded(item, &encoding);
    if (!contracts::succeeded(result)) return result;
    if (encoding != contracts::QolItemCountEncoding::kEncoded &&
        encoding != contracts::QolItemCountEncoding::kNotEncoded) {
        return error_result(kQolErrUnsupported, "item count encoding unavailable");
    }
    *out_encoding = encoding;
    return contracts::ok();
}

QolResult read_item_ref_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    // 中立 provider 契约：只校验负 bag/slot 与固定槽界；逻辑袋号认领交由 provider。
    if (out == nullptr || bag < 0 || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    return qol::game::inventory_item_ref_at(bag, slot, out);
}

QolResult read_inventory_extension_access(contracts::QolInventoryExtensionAccess* out_access) {
    return inventory_extension_get_access(out_access);
}

QolResult read_physical_item_at(int32_t bag, int32_t slot, contracts::QolItemHandle* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid item output");
    void* item = nullptr;
    const QolResult result = physical_item_at(bag, slot, &item);
    if (!contracts::succeeded(result)) return result;
    ItemHandleError handle_error = ItemHandleError::kNone;
    contracts::QolItemHandle handle = lookup_item(item);
    if (handle.slot == 0) {
        handle = register_item(item, contracts::kQolItemHandleOwnerGame, &handle_error);
    }
    if (handle.slot == 0) return error_result(kQolErrNotReady, "item handle unavailable");
    *out = handle;
    return contracts::ok();
}

QolResult read_detail_item_context(contracts::QolItemDetailContext* out_context) {
    return qol::game::detail_item_context(out_context);
}

QolResult copy_json(const std::string& json, char* out, uint32_t capacity, uint32_t* out_size) {
    if (out == nullptr || out_size == nullptr || json.size() >= capacity) {
        return error_result(kQolErrInvalidArgument, "invalid json buffer");
    }
    std::memcpy(out, json.data(), json.size());
    out[json.size()] = '\0';
    *out_size = static_cast<uint32_t>(json.size());
    return contracts::ok();
}

QolResult read_inventory_json(char* out, uint32_t capacity, uint32_t* out_size) {
    return copy_json(build_inventory_json(), out, capacity, out_size);
}

QolResult read_inventory_item_view(contracts::QolItemHandle handle,
                                   contracts::QolItemInventoryView* out) {
    void* item = nullptr;
    const QolResult resolution = resolve_item_handle(handle, &item);
    if (!contracts::succeeded(resolution)) return resolution;
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid item view args");
    }
    // A resolved handle can outlive the native item when a lifecycle path fails
    // to invalidate it. Do not dereference an unmapped pointer here; report the
    // stale handle so the caller can skip it instead of taking SIGSEGV.
    if (!game_memory_accessible(item, I_TYPE + sizeof(uint16_t), 'r')) {
        return error_result(kQolErrNotFound, "stale item pointer");
    }
    *out = {};
    const auto* bytes = static_cast<const uint8_t*>(item);
    const uint16_t type_flags = *reinterpret_cast<const uint16_t*>(bytes + I_TYPE);
    out->type_flags = type_flags;
    out->raw_rarity = (type_flags >> 2) & 0x0F;
    if (fn_get_bit != nullptr) {
        out->fields |= contracts::kQolItemInventoryHasCategory;
        out->category = static_cast<int32_t>(fn_get_bit(type_flags, 15, 6));
    }
    out->count_supported = fn_get_cumulate_count != nullptr ||
                           item_count_api_if_resident() != nullptr;
    if (const contracts::QolItemCountApi* count_api = item_count_api_if_resident();
        count_api != nullptr && count_api->effective != nullptr) {
        uint32_t effective = 0;
        if (contracts::succeeded(count_api->effective(item, &effective))) {
            out->effective_count = static_cast<int32_t>(effective);
        } else {
            out->count_supported = 0;
        }
    } else if (fn_get_cumulate_count != nullptr) {
        out->effective_count = fn_get_cumulate_count(item);
    }
    out->equip = item_is_equip(const_cast<void*>(item)) ? 1 : 0;
    if (fn_get_rarity != nullptr) {
        out->fields |= contracts::kQolItemInventoryHasRarity;
        out->rarity = fn_get_rarity(const_cast<void*>(item));
    }
    if (fn_get_damage != nullptr) {
        out->fields |= contracts::kQolItemInventoryHasDamage;
        out->damage = fn_get_damage(const_cast<void*>(item));
    }
    if (fn_get_defense != nullptr) {
        out->fields |= contracts::kQolItemInventoryHasDefense;
        out->defense = fn_get_defense(const_cast<void*>(item));
    }
    out->magic_rate = bytes[I_MAGIC_RATE];
    if (fn_item_get_ability_level != nullptr) {
        out->fields |= contracts::kQolItemInventoryHasNeedLevel;
        out->need_level = fn_item_get_ability_level(const_cast<void*>(item));
    }
    out->socket = bytes[I_SOCKET];
    out->socket_filled = out->socket & 0x0F;
    out->socket_total = (out->socket >> 4) & 0x0F;
    const uint16_t enchant = *reinterpret_cast<const uint16_t*>(bytes + I_ENCHANT);
    out->enchant = enchant;
    out->enchant_remaining = (enchant >> 2) & 0x0F;
    out->chaos = (enchant & 1u) != 0 ? 1 : 0;
    out->enchant_id = (enchant >> 11) & 0x1F;
    out->enchant_level = (enchant >> 6) & 0x1F;
    const uint32_t count_field = *reinterpret_cast<const uint32_t*>(bytes + I_COUNT);
    out->chaos_level = count_field & 0xFF;
    out->chaos_rate = (count_field >> 8) & 0xFF;
    out->jewel_value = static_cast<int32_t>(count_field & 0x7FF);
    out->jewel_attribute_type = static_cast<int32_t>((count_field >> 18) & 0x3F);
    out->fields |= contracts::kQolItemInventoryHasJewelValue |
                   contracts::kQolItemInventoryHasJewelAttributeType;
    const auto* option = *reinterpret_cast<const uint8_t* const*>(bytes + I_OPTION_LIST);
    out->fields |= contracts::kQolItemInventoryHasOptionCompleteness |
                   contracts::kQolItemInventoryHasNativeOptions |
                   contracts::kQolItemInventoryHasJewelOptions;
    // A stale item can still be mapped while its option-list pointer points into
    // a freed allocation. Validate every chain node before dereferencing it.
    const auto option_readable = [](const uint8_t* node) {
        return node == nullptr ||
               game_memory_accessible(node, O_NEXT + sizeof(void*), 'r');
    };
    if (!option_readable(option)) {
        out->fields |= contracts::kQolItemInventoryOptionsTruncated;
        option = nullptr;
    }
    while (option != nullptr && out->option_count < contracts::kQolItemInventoryOptionCapacity) {
        if (!option_readable(option)) {
            out->fields |= contracts::kQolItemInventoryOptionsTruncated;
            break;
        }
        const uint16_t index = *reinterpret_cast<const uint16_t*>(option + O_INDEX);
        const contracts::QolItemInventoryOptionView entry{
            *reinterpret_cast<const int16_t*>(option + O_VALUE), index & 0x7F,
            (index >> 13) & 0x7};
        out->options[out->option_count++] = entry;
        // 合并列表保持原序；分词列表只按已知 type 投影。32 项上限与合并列表一致，
        // 因此单个分词列表不会因总量而溢出。
        if (entry.type == static_cast<int32_t>(contracts::QolItemInventoryOptionType::kNative) &&
            out->native_option_count < contracts::kQolItemInventoryNativeOptionCapacity) {
            out->native_options[out->native_option_count++] = entry;
        } else if (entry.type ==
                       static_cast<int32_t>(contracts::QolItemInventoryOptionType::kJewel) &&
                   out->jewel_option_count < contracts::kQolItemInventoryJewelOptionCapacity) {
            out->jewel_options[out->jewel_option_count++] = entry;
        }
        const auto* next = *reinterpret_cast<const uint8_t* const*>(option + O_NEXT);
        if (out->option_count == contracts::kQolItemInventoryOptionCapacity) {
            if (next != nullptr) {
                out->fields |= contracts::kQolItemInventoryOptionsTruncated;
            }
            break;
        }
        if (!option_readable(next)) {
            out->fields |= contracts::kQolItemInventoryOptionsTruncated;
            break;
        }
        option = next;
    }
    return contracts::ok();
}

QolResult read_money(int64_t* out) {
    if (out == nullptr || !world_money_ready()) return not_ready();
    *out = world_money();
    return contracts::ok();
}

QolResult read_shop_items_json(char* out, uint32_t capacity, uint32_t* out_size) {
    return copy_json(qol::game::shop_items_json(), out, capacity, out_size);
}

QolResult read_in_world(int32_t* out) {
    if (out == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    *out = game_in_world() ? 1 : 0;
    return contracts::ok();
}

// 启动期下发的静态名称表：只在启动期写入，运行期只读；字符串是 JSON 已转义、不含外层引号
// 的文本，handler 直接拼接即可与既有 Kotlin JSONObject 序列化保持一致。
constexpr uint32_t kTextKindCount = 5;
std::unordered_map<int32_t, std::string> g_item_text[kTextKindCount];
std::mutex g_item_text_mutex;
std::atomic<bool> g_item_text_ready{false};

QolResult read_static_text(int32_t kind, int32_t id, const char** out_text) {
    if (out_text == nullptr || kind < 0 || kind >= static_cast<int32_t>(kTextKindCount)) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    *out_text = nullptr;
    std::lock_guard<std::mutex> lock(g_item_text_mutex);
    const auto& table = g_item_text[kind];
    const auto it = table.find(id);
    if (it == table.end()) return error_result(kQolErrNotFound, "text not found");
    *out_text = it->second.c_str();
    return contracts::ok();
}

QolResult read_static_ready(int32_t* out_ready) {
    if (out_ready == nullptr) return error_result(kQolErrInvalidArgument, "invalid argument");
    *out_ready = g_item_text_ready.load() ? 1 : 0;
    return contracts::ok();
}

const contracts::QolItemPort kItemPort{
    contracts::kQolPortItemVersion,
    sizeof(contracts::QolItemPort),
    {&read_category, &read_real_equip, &read_jewel_category, &read_no_sell, &read_sell_price,
     &read_cumulate_count, &read_item_ref_at, &read_physical_item_at, &read_detail_item_context,
      &read_inventory_json, &read_money, &read_shop_items_json, &read_in_world,
       &read_static_text, &read_static_ready, &read_inventory_item_view,
         &read_effective_cumulate_count, &read_inventory_extension_access, &read_count_limit,
         &read_count_encoding, &read_quote},
};

}  // namespace

const contracts::QolItemPort& item_port() { return kItemPort; }

// 解析 Kotlin 下发的行式负载：每行 `<kind>\t<id>\t<escaped-text>`；kind 为 N/T/O/E。
// 只按真实制表符与换行切分；文本本身已被 JSON 转义（含 \t/\n 也变成两字符），不含真实控制符。
void item_port_load_static_text(const char* payload) {
    if (payload == nullptr) return;
    std::unordered_map<int32_t, std::string> parsed[kTextKindCount];
    const char* cursor = payload;
    while (*cursor != '\0') {
        const char* line_end = std::strchr(cursor, '\n');
        const size_t line_length = line_end == nullptr ? std::strlen(cursor)
                                                       : static_cast<size_t>(line_end - cursor);
        // 最短合法行 "N\t0\t"（空文本）长度为 4；值本身已 JSON 转义，不含真实制表符/换行。
        if (line_length >= 4 && cursor[1] == '\t') {
            int kind = -1;
            switch (cursor[0]) {
                case 'N': kind = 0; break;
                case 'T': kind = 1; break;
                case 'O': kind = 2; break;
                case 'E': kind = 3; break;
                case 'S': kind = 4; break;
                default: break;
            }
            const char* id_start = cursor + 2;
            const char* id_end = static_cast<const char*>(
                memchr(id_start, '\t', line_length - 2));
            if (kind >= 0 && id_end != nullptr) {
                const int32_t id = static_cast<int32_t>(std::strtol(id_start, nullptr, 10));
                const char* value_start = id_end + 1;
                const size_t value_length = line_length - static_cast<size_t>(value_start - cursor);
                parsed[kind][id] = std::string(value_start, value_length);
            }
        }
        if (line_end == nullptr) break;
        cursor = line_end + 1;
    }
    {
        std::lock_guard<std::mutex> lock(g_item_text_mutex);
        for (uint32_t kind = 0; kind < kTextKindCount; ++kind) {
            g_item_text[kind] = std::move(parsed[kind]);
        }
    }
    g_item_text_ready.store(true);
}

}  // namespace qol::game
