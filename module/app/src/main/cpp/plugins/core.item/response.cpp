#include "response.h"

#include <cstring>
#include <string>

#include "json_lite.h"

namespace qol::features::core_item {
namespace {

using contracts::QolItemStaticTextKind;

// 组装 JSON 对象正文；调用方按迁移前字段顺序逐项写入。
struct Obj {
    std::string body;
    bool first = true;

    void key(const char* name) {
        if (!first) body += ",";
        first = false;
        body += "\"";
        body += name;
        body += "\":";
    }
    void integer(const char* name, long long value) {
        key(name);
        body += std::to_string(value);
    }
    void boolean(const char* name, bool value) {
        key(name);
        body += value ? "true" : "false";
    }
    void null_value(const char* name) {
        key(name);
        body += "null";
    }
    // escaped 为 JSON 已转义、不含外层引号的文本；nullptr → null。
    void text(const char* name, const char* escaped) {
        key(name);
        if (escaped == nullptr) {
            body += "null";
        } else {
            body += "\"";
            body += escaped;
            body += "\"";
        }
    }
    void raw(const char* name, const std::string& json) {
        key(name);
        body += json;
    }
    std::string object() const { return "{" + body + "}"; }
};

const char* static_text(const contracts::QolItemPort* port, QolItemStaticTextKind kind, int32_t id) {
    if (port == nullptr || port->read.static_text == nullptr) return nullptr;
    const char* text = nullptr;
    if (!contracts::succeeded(port->read.static_text(static_cast<int32_t>(kind), id, &text))) {
        return nullptr;
    }
    return text;
}

bool int_of(const JsonValue& value, long long* out) { return value.as_int(out); }

bool member_int(const JsonValue& object, const char* key, long long* out) {
    const JsonValue* value = object.find(key);
    return value != nullptr && value->as_int(out);
}

bool member_bool(const JsonValue& object, const char* key, bool* out) {
    const JsonValue* value = object.find(key);
    return value != nullptr && value->as_bool(out);
}

// 迁移前 rarityPrefix（StaticData.kt）：raw_rarity → 品质前缀。
const char* rarity_prefix(long long raw_rarity) {
    switch (raw_rarity) {
        case 0: return "生锈的 ";
        case 1: return "陈旧的 ";
        case 3: return "太古的 ";
        case 4: return "锐利的 ";
        case 5: return "打磨的 ";
        case 6: return "工匠的 ";
        case 7: return "钢铁 ";
        case 8: return "钛金 ";
        case 9: return "秘银 ";
        default: return "";
    }
}

// 迁移前 rarityTierName（StaticData.kt）：GetRarity 0-4 → 白/绿/蓝/黄/紫。
const char* rarity_tier(long long rarity) {
    switch (rarity) {
        case 0: return "白";
        case 1: return "绿";
        case 2: return "蓝";
        case 3: return "黄";
        case 4: return "紫";
        default: return "";
    }
}

// magic_rate 显示 = 原始值/100（110 → 1.1）；org.json 对整数倍输出 "0"。
std::string magic_rate_text(long long value) {
    const long long integer = value / 100;
    const long long remainder = value % 100;
    if (remainder == 0) return std::to_string(integer);
    if (remainder % 10 == 0) {
        return std::to_string(integer) + "." + static_cast<char>('0' + remainder / 10);
    }
    std::string fraction = std::to_string(remainder);
    if (fraction.size() == 1) fraction = "0" + fraction;
    return std::to_string(integer) + "." + fraction;
}

std::string option_list(const JsonValue& item, const contracts::QolItemPort* port, int64_t type) {
    const JsonValue* ids = item.find("option_ids");
    if (ids == nullptr || ids->type != JsonValue::Type::kArray) return "[]";
    const JsonValue* types = item.find("option_types");
    const JsonValue* values = item.find("options");
    std::string out = "[";
    bool first = true;
    for (size_t i = 0; i < ids->items.size(); ++i) {
        long long id = -1;
        if (!ids->items[i].as_int(&id) || id < 0) continue;
        long long option_type = 0;
        if (types != nullptr && types->type == JsonValue::Type::kArray &&
            i < types->items.size()) {
            (void)types->items[i].as_int(&option_type);
        }
        if (option_type != type) continue;
        long long value = 0;
        if (values != nullptr && values->type == JsonValue::Type::kArray &&
            i < values->items.size()) {
            (void)values->items[i].as_int(&value);
        }
        if (!first) out += ",";
        first = false;
        const char* name = static_text(port, QolItemStaticTextKind::kOptionName,
                                       static_cast<int32_t>(id));
        Obj entry;
        entry.integer("id", id);
        entry.text("name", name == nullptr ? "" : name);
        entry.integer("value", value);
        out += entry.object();
    }
    out += "]";
    return out;
}

// 迁移前 NameInjectorCore.restructureItem 的字段顺序与取值。
std::string enrich_item(const JsonValue& item, const contracts::QolItemPort* port) {
    long long category = -1;
    const bool has_category = member_int(item, "category", &category);

    Obj out;
    long long slot = 0;
    if (member_int(item, "slot", &slot)) out.integer("slot", slot); else out.null_value("slot");

    const char* base_name = has_category
                                ? static_text(port, QolItemStaticTextKind::kItemName,
                                              static_cast<int32_t>(category))
                                : nullptr;
    if (base_name != nullptr) {
        const char* prefix = "";
        bool equip = false;
        long long raw_rarity = -1;
        if (member_bool(item, "equip", &equip) && equip &&
            member_int(item, "raw_rarity", &raw_rarity)) {
            prefix = rarity_prefix(raw_rarity);
        }
        const std::string name = std::string(prefix) + base_name;
        out.text("name", name.c_str());
    } else {
        out.null_value("name");
    }

    if (has_category) out.integer("category", category); else out.null_value("category");

    long long count = 0;
    if (member_int(item, "count", &count)) out.integer("count", count); else out.null_value("count");

    const char* item_type = has_category
                                ? static_text(port, QolItemStaticTextKind::kItemType,
                                              static_cast<int32_t>(category))
                                : nullptr;
    out.text("item_type", item_type == nullptr ? "consumable" : item_type);

    long long need_level = 0;
    if (member_int(item, "need_level", &need_level)) {
        out.integer("need_level", need_level < 0 ? 0 : need_level);
    } else {
        out.integer("need_level", 0);
    }

    long long rarity = 0;
    const bool has_rarity = member_int(item, "rarity", &rarity);
    if (has_rarity) out.integer("rarity", rarity); else out.null_value("rarity");

    if (has_rarity) {
        const char* tier = rarity_tier(rarity);
        out.text("rarity_tier", tier[0] == '\0' ? nullptr : tier);
    } else {
        out.null_value("rarity_tier");
    }

    Obj base;
    long long damage = 0;
    if (member_int(item, "damage", &damage)) base.integer("damage", damage); else base.null_value("damage");
    long long defense = 0;
    if (member_int(item, "defense", &defense)) base.integer("defense", defense); else base.null_value("defense");
    long long magic_rate = 0;
    if (member_int(item, "magic_rate", &magic_rate)) {
        base.raw("magic_rate", magic_rate_text(magic_rate));
    } else {
        base.null_value("magic_rate");
    }
    out.raw("base", base.object());

    out.raw("bonus", option_list(item, port, 0));

    Obj gem;
    long long socket_total = 0;
    if (member_int(item, "socket_total", &socket_total)) gem.integer("total_slots", socket_total);
    else gem.null_value("total_slots");
    gem.raw("slots", option_list(item, port, 1));
    out.raw("gem", gem.object());

    Obj chaos;
    bool is_chaos = false;
    member_bool(item, "chaos", &is_chaos);
    chaos.boolean("is_chaos", is_chaos);
    long long chaos_level = 0;
    if (member_int(item, "chaos_level", &chaos_level)) chaos.integer("level", chaos_level); else chaos.null_value("level");
    long long chaos_rate = 0;
    if (member_int(item, "chaos_rate", &chaos_rate)) chaos.integer("rate", chaos_rate); else chaos.null_value("rate");
    out.raw("chaos", chaos.object());

    Obj enchant;
    long long enchant_id = 0;
    const bool has_enchant_id = member_int(item, "enchant_id", &enchant_id);
    if (has_enchant_id) enchant.integer("id", enchant_id); else enchant.null_value("id");
    long long enchant_level = 0;
    if (member_int(item, "enchant_level", &enchant_level)) enchant.integer("level", enchant_level); else enchant.null_value("level");
    if (has_enchant_id && enchant_id > 0) {
        const char* effect = static_text(port, QolItemStaticTextKind::kEnchantEffect,
                                         static_cast<int32_t>(enchant_id));
        enchant.text("effect", effect);
    } else {
        enchant.null_value("effect");
    }
    out.raw("enchant", enchant.object());

    return out.object();
}

const JsonValue* bags_of(const JsonValue& root) {
    if (root.type != JsonValue::Type::kObject) return nullptr;
    const JsonValue* bags = root.find("bags");
    if (bags == nullptr || bags->type != JsonValue::Type::kArray) return nullptr;
    return bags;
}

const JsonValue* items_of(const JsonValue& bag) {
    const JsonValue* items = bag.find("items");
    if (items == nullptr || items->type != JsonValue::Type::kArray) return nullptr;
    return items;
}

}  // namespace

bool enrich_inventory(const char* json, uint32_t size, const contracts::QolItemPort* port,
                      std::string* out) {
    if (out == nullptr) return false;
    JsonValue root;
    if (!json_parse(json, size, &root)) return false;
    const JsonValue* bags = bags_of(root);
    if (bags == nullptr) return false;

    std::string result = "{\"bags\":[";
    bool first_bag = true;
    for (const JsonValue& bag : bags->items) {
        if (!first_bag) result += ",";
        first_bag = false;
        long long bag_id = -1;
        (void)member_int(bag, "bag", &bag_id);
        result += "{\"bag\":" + std::to_string(bag_id) + ",\"items\":[";
        const JsonValue* items = items_of(bag);
        if (items != nullptr) {
            bool first_item = true;
            for (const JsonValue& item : items->items) {
                if (!first_item) result += ",";
                first_item = false;
                result += enrich_item(item, port);
            }
        }
        long long capacity = 16;
        (void)member_int(bag, "capacity", &capacity);
        long long slot_count = 0;
        (void)member_int(bag, "slot_count", &slot_count);
        result += "],\"capacity\":" + std::to_string(capacity) +
                  ",\"slot_count\":" + std::to_string(slot_count) + "}";
    }
    result += "]}";
    *out = std::move(result);
    return true;
}

bool enrich_items_list(const char* json, uint32_t size, const contracts::QolItemPort* port,
                       std::string* out) {
    if (out == nullptr) return false;
    JsonValue root;
    if (!json_parse(json, size, &root)) return false;
    const JsonValue* bags = bags_of(root);
    if (bags == nullptr) return false;

    std::string result = "{\"items\":[";
    bool first = true;
    for (const JsonValue& bag : bags->items) {
        const JsonValue* items = items_of(bag);
        if (items == nullptr) continue;
        for (const JsonValue& item : items->items) {
            if (!first) result += ",";
            first = false;
            result += enrich_item(item, port);
        }
    }
    result += "]}";
    *out = std::move(result);
    return true;
}

bool enrich_bag_info(const char* json, uint32_t size, int32_t bag, std::string* out, bool* found) {
    if (out == nullptr || found == nullptr) return false;
    *found = false;
    JsonValue root;
    if (!json_parse(json, size, &root)) return false;
    const JsonValue* bags = bags_of(root);
    if (bags == nullptr) return false;
    for (const JsonValue& entry : bags->items) {
        long long bag_id = -1;
        if (!member_int(entry, "bag", &bag_id) || bag_id != bag) continue;
        long long capacity = -1;
        (void)member_int(entry, "capacity", &capacity);
        long long slot_count = -1;
        (void)member_int(entry, "slot_count", &slot_count);
        *out = "{\"bag\":" + std::to_string(bag_id) + ",\"capacity\":" +
               std::to_string(capacity) + ",\"slot_count\":" + std::to_string(slot_count) + "}";
        *found = true;
        return true;
    }
    return true;
}

bool enrich_bag_slot(const char* json, uint32_t size, int32_t bag, int32_t slot,
                     const contracts::QolItemPort* port, std::string* out, bool* bag_found) {
    if (out == nullptr || bag_found == nullptr) return false;
    *bag_found = false;
    JsonValue root;
    if (!json_parse(json, size, &root)) return false;
    const JsonValue* bags = bags_of(root);
    if (bags == nullptr) return false;
    for (const JsonValue& entry : bags->items) {
        long long bag_id = -1;
        if (!member_int(entry, "bag", &bag_id) || bag_id != bag) continue;
        *bag_found = true;
        const JsonValue* items = items_of(entry);
        if (items == nullptr) return false;
        for (const JsonValue& item : items->items) {
            long long item_slot = -1;
            if (!member_int(item, "slot", &item_slot) || item_slot != slot) continue;
            *out = enrich_item(item, port);
            return true;
        }
        *out = "null";
        return true;
    }
    return true;
}

bool enrich_shop_items(const char* json, uint32_t size, const contracts::QolItemPort* port,
                       std::string* out) {
    if (out == nullptr) return false;
    JsonValue root;
    if (!json_parse(json, size, &root)) return false;
    if (root.type != JsonValue::Type::kObject) return false;
    const JsonValue* items = root.find("items");
    if (items == nullptr || items->type != JsonValue::Type::kArray) return false;

    std::string result = "{\"items\":[";
    bool first = true;
    for (const JsonValue& item : items->items) {
        if (!first) result += ",";
        first = false;
        // 商店端不重排字段：Kotlin 仅在原对象末尾追加 name，故只输出存在的原生字段。
        Obj entry;
        for (const auto& key : {"slot", "category", "count", "price"}) {
            long long value = 0;
            if (member_int(item, key, &value)) entry.integer(key, value);
        }
        long long category = -1;
        if (member_int(item, "category", &category) && category >= 0) {
            const char* name = static_text(port, QolItemStaticTextKind::kItemName,
                                          static_cast<int32_t>(category));
            if (name != nullptr) entry.text("name", name);
        }
        result += entry.object();
    }
    result += "]}";
    *out = std::move(result);
    return true;
}

}  // namespace qol::features::core_item
