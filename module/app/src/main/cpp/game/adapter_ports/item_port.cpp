#include "game/adapter_ports/item_port.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "contracts/game_ports/ports.h"
#include "game/access/attr_range_ui.h"
#include "game/access/game_access.h"
#include "game/access/inventory.h"
#include "game/access/item_class.h"
#include "game/adapter_ports/item_count_facade.h"
#include "game/access/shop.h"
#include "game/access/world.h"
#include "game/read/item_class.h"
#include "game/state/game_state.h"

// JSON 构造器仍供未迁移的 game-system write/cache 路径使用；core.item owner 不用此入口。
std::string build_inventory_json();

namespace qol::game {
namespace {

using QolResult = ::QolResult;

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult not_ready() { return error_result(kQolErrNotReady, "not ready"); }

QolResult read_category(const void* item, int32_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    const int value = attr_item_category(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_real_equip(const void* item, int32_t* out) {
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

QolResult read_sell_price(const void* item, int64_t* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    const int value = item_sell_price(item);
    if (value < 0) return not_ready();
    *out = value;
    return contracts::ok();
}

QolResult read_cumulate_count(const void* item, int32_t* out) {
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

QolResult read_effective_cumulate_count(const void* item, int32_t* out) {
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

QolResult read_item_ref_at(int32_t bag, int32_t slot, contracts::QolInventoryItemRef* out) {
    if (out == nullptr || bag < 0 || bag > 10 || slot < 0 || slot >= 16) {
        return error_result(kQolErrInvalidArgument, "invalid argument");
    }
    return inventory_item_ref_at(bag, slot, out);
}

QolResult read_physical_item_at(int32_t bag, int32_t slot, void** out) {
    return physical_item_at(bag, slot, out);
}

QolResult read_detail_context(int32_t* out_desc_type, int32_t* out_bag, void** out_control) {
    return detail_context(out_desc_type, out_bag, out_control);
}

QolResult read_cursor_index(const void* control, int32_t* out_slot) {
    return cursor_index(control, out_slot);
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

QolResult read_inventory_item_view(const void* item, contracts::QolItemInventoryView* out) {
    if (item == nullptr || out == nullptr) {
        return error_result(kQolErrInvalidArgument, "invalid item view args");
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
    out->chaos = (enchant & 1u) != 0 ? 1 : 0;
    out->enchant_id = (enchant >> 11) & 0x1F;
    out->enchant_level = (enchant >> 6) & 0x1F;
    const uint32_t count_field = *reinterpret_cast<const uint32_t*>(bytes + I_COUNT);
    out->chaos_level = count_field & 0xFF;
    out->chaos_rate = (count_field >> 8) & 0xFF;
    const auto* option = *reinterpret_cast<const uint8_t* const*>(bytes + I_OPTION_LIST);
    while (option != nullptr && out->option_count < contracts::kQolItemInventoryOptionCapacity) {
        const uint16_t index = *reinterpret_cast<const uint16_t*>(option + O_INDEX);
        out->options[out->option_count++] = contracts::QolItemInventoryOptionView{
            *reinterpret_cast<const int16_t*>(option + O_VALUE), index & 0x7F,
            (index >> 13) & 0x7};
        option = *reinterpret_cast<const uint8_t* const*>(option + O_NEXT);
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
    {&read_category, &read_real_equip, &read_jewel_category, &read_no_sell, &read_sell_price,
     &read_cumulate_count, &read_item_ref_at, &read_physical_item_at, &read_detail_context,
      &read_cursor_index, &read_inventory_json, &read_money, &read_shop_items_json, &read_in_world,
       &read_static_text, &read_static_ready, &read_inventory_item_view,
       &read_effective_cumulate_count},
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
