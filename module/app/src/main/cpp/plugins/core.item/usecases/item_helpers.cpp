#include "plugins/core.item/runtime/core_item_internal.h"

#include <cstring>
#include <string>

#include "contracts/extension_bag.h"

namespace qol::features::core_item {

QolResult not_ready() {
    return contracts::error(kQolErrNotReady, "item facts not ready", 20);
}

QolResult error_result(int32_t code, const char* message) {
    return contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}
bool copy_buffer(char* destination, uint32_t capacity, uint32_t* destination_size,
                 const char* source, uint32_t source_size) {
    if (destination == nullptr || destination_size == nullptr || source == nullptr ||
        source_size >= capacity) {
        return false;
    }
    std::memcpy(destination, source, source_size);
    destination[source_size] = '\0';
    *destination_size = source_size;
    return true;
}
InventoryExtensionAccessResult inventory_extension() {
    InventoryExtensionAccessResult result;
    if (g_port == nullptr || g_port->read.inventory_extension_access == nullptr) return result;
    contracts::QolInventoryExtensionAccess access{};
    if (!contracts::succeeded(g_port->read.inventory_extension_access(&access))) return result;
    result.provider = access.provider;
    result.available = access.available != 0;
    return result;
}

bool effective_item_count(contracts::QolItemHandle item, bool original_count_supported,
                          uint32_t* out_count,
                          bool* out_present) {
    if (item.slot == 0 || item.generation == 0 || out_count == nullptr || out_present == nullptr) {
        return false;
    }
    *out_present = false;
    if (g_port != nullptr && g_port->read.effective_cumulate_count != nullptr) {
        int32_t effective = 0;
        const QolResult result = g_port->read.effective_cumulate_count(item, &effective);
        if (contracts::succeeded(result)) {
            *out_count = static_cast<uint32_t>(effective);
            *out_present = true;
            return true;
        }
    }

    if (g_port != nullptr && g_port->read.cumulate_count != nullptr) {
        int32_t original_count = 0;
        const QolResult result = g_port->read.cumulate_count(item, &original_count);
        if (contracts::succeeded(result)) {
            *out_count = static_cast<uint32_t>(original_count);
            *out_present = true;
            return true;
        }
        if (original_count_supported) return false;
    } else if (original_count_supported) {
        return false;
    }
    return true;
}

bool inventory_option_counts_valid(const contracts::QolItemInventoryView& item) {
    return item.native_option_count <= contracts::kQolItemInventoryNativeOptionCapacity &&
           item.jewel_option_count <= contracts::kQolItemInventoryJewelOptionCapacity &&
           item.enchant_option_count <= contracts::kQolItemInventoryEnchantOptionCapacity;
}

void append_inventory_item(std::string* out, int32_t slot,
                           const contracts::QolItemInventoryView& item, bool count_present,
                           uint32_t count) {
    out->append("{\"slot\":");
    *out += std::to_string(slot);
    out->append(",\"type_flags\":");
    *out += std::to_string(item.type_flags);
    out->append(",\"raw_rarity\":");
    *out += std::to_string(item.raw_rarity);
    if ((item.fields & contracts::kQolItemInventoryHasCategory) != 0) {
        out->append(",\"category\":");
        *out += std::to_string(item.category);
    }
    if (count_present) {
        out->append(",\"count\":");
        *out += std::to_string(count);
    }
    out->append(item.equip != 0 ? ",\"equip\":true" : ",\"equip\":false");
    if ((item.fields & contracts::kQolItemInventoryHasRarity) != 0) {
        out->append(",\"rarity\":");
        *out += std::to_string(item.rarity);
    }
    if ((item.fields & contracts::kQolItemInventoryHasDamage) != 0) {
        out->append(",\"damage\":");
        *out += std::to_string(item.damage);
    }
    if ((item.fields & contracts::kQolItemInventoryHasDefense) != 0) {
        out->append(",\"defense\":");
        *out += std::to_string(item.defense);
    }
    out->append(",\"magic_rate\":");
    *out += std::to_string(item.magic_rate);
    if ((item.fields & contracts::kQolItemInventoryHasNeedLevel) != 0) {
        out->append(",\"need_level\":");
        *out += std::to_string(item.need_level);
    }
    out->append(",\"socket\":");
    *out += std::to_string(item.socket);
    out->append(",\"socket_filled\":");
    *out += std::to_string(item.socket_filled);
    out->append(",\"socket_total\":");
    *out += std::to_string(item.socket_total);
    out->append(",\"enchant\":");
    *out += std::to_string(item.enchant);
    out->append(item.chaos != 0 ? ",\"chaos\":true" : ",\"chaos\":false");
    out->append(",\"enchant_id\":");
    *out += std::to_string(item.enchant_id);
    out->append(",\"enchant_level\":");
    *out += std::to_string(item.enchant_level);
    out->append(",\"chaos_level\":");
    *out += std::to_string(item.chaos_level);
    out->append(",\"chaos_rate\":");
    *out += std::to_string(item.chaos_rate);

    // JSON 保持既有 options/option_ids/option_types 三个平行数组；条目按原生、宝石
    // 列表顺序拼接。附魔列表当前无数据，不参与投影；未知 type 不进入任何列表。
    const contracts::QolItemInventoryOptionView* const lists[] = {
        item.native_options, item.jewel_options};
    const uint32_t counts[] = {item.native_option_count, item.jewel_option_count};
    const auto append_option_field = [&](auto value_of) {
        uint32_t emitted = 0;
        for (uint32_t list = 0; list < 2; ++list) {
            for (uint32_t index = 0; index < counts[list]; ++index) {
                if (emitted++ != 0) out->push_back(',');
                *out += std::to_string(value_of(lists[list][index]));
            }
        }
    };
    out->append(",\"options\":[");
    append_option_field([](const auto& option) { return option.value; });
    out->append("],\"option_ids\":[");
    append_option_field([](const auto& option) { return option.id; });
    out->append("],\"option_types\":[");
    append_option_field([](const auto& option) { return option.type; });
    out->append("]}");
}


}
