#include "core_settings_config_store.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace qol::features::core_settings {
namespace {

constexpr std::size_t kMaxConfigBytes = 1024 * 1024;

bool valid_utf8(std::string_view value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7f) { ++index; continue; }
        std::size_t length = 0;
        unsigned char second_min = 0x80;
        unsigned char second_max = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) length = 2;
        else if (first == 0xe0) { length = 3; second_min = 0xa0; }
        else if ((first >= 0xe1 && first <= 0xec) || (first >= 0xee && first <= 0xef)) length = 3;
        else if (first == 0xed) { length = 3; second_max = 0x9f; }
        else if (first == 0xf0) { length = 4; second_min = 0x90; }
        else if (first >= 0xf1 && first <= 0xf3) length = 4;
        else if (first == 0xf4) { length = 4; second_max = 0x8f; }
        else return false;
        if (value.size() - index < length) return false;
        const unsigned char second = static_cast<unsigned char>(value[index + 1]);
        if (second < second_min || second > second_max) return false;
        for (std::size_t offset = 2; offset < length; ++offset) {
            const unsigned char continuation = static_cast<unsigned char>(value[index + offset]);
            if (continuation < 0x80 || continuation > 0xbf) return false;
        }
        index += length;
    }
    return true;
}

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    bool read_config(ModuleConfig* config, bool* needs_migration,
                     std::vector<std::pair<std::string, JsonValue>>* extras) {
        bool migration_keys[8]{};
        bool has_legacy_key = false;
        bool has_unknown_key = false;
        whitespace();
        if (!consume('{')) return false;
        whitespace();
        if (!peek('}')) {
            for (;;) {
                std::string key;
                if (!read_string(&key)) return false;
                whitespace();
                if (!consume(':')) return false;
                whitespace();
                for (std::size_t index = 0; index < kMigrationKeys.size(); ++index) {
                    if (key == kMigrationKeys[index]) migration_keys[index] = true;
                }
                if (key == "jewelBatchMix") has_legacy_key = true;
                else if (!is_known(key)) has_unknown_key = true;
                if (is_known(key)) {
                    if (!read_known(key, config)) return false;
                } else {
                    JsonValue value;
                    if (!read_value(&value, 0)) return false;
                    extras->emplace_back(key, std::move(value));
                }
                whitespace();
                if (consume('}')) break;
                if (!consume(',')) return false;
                whitespace();
            }
        } else {
            consume('}');
        }
        whitespace();
        if (position_ != input_.size()) return false;
        *needs_migration = has_legacy_key || has_unknown_key;
        for (bool present : migration_keys) *needs_migration = *needs_migration || !present;
        return true;
    }

    bool read_value_document(JsonValue* value) {
        whitespace();
        if (!read_value(value, 0)) return false;
        whitespace();
        return position_ == input_.size();
    }

private:
    bool is_known(const std::string& key) const {
        return key == "listenAddress" || key == "listenPort" || key == "stackLimitIncrease" ||
               key == "moveMergeEnabled" || key == "opEnabled" || key == "extensionBagEnabled" ||
               key == "customRecipeEnabled" || key == "simpleModeEnabled" ||
               key == "pushAllEnabled" || key == "autoSellEnabled" || key == "apiEnabled" ||
               key == "debugLogEnabled";
    }

    static constexpr std::array<std::string_view, 8> kMigrationKeys = {
        "moveMergeEnabled", "extensionBagEnabled", "customRecipeEnabled", "simpleModeEnabled",
        "pushAllEnabled", "autoSellEnabled", "apiEnabled", "debugLogEnabled"};

    bool read_known(const std::string& key, ModuleConfig* c) {
        if (key == "listenAddress") {
            std::string value;
            if (!read_string(&value)) return skip_value(0);
            if (value.find_first_not_of(" \t\r\n\f\v") != std::string::npos)
                c->listen_address = std::move(value);
            return true;
        }
        if (key == "listenPort") {
            const std::size_t start = position_;
            int64_t value = 0;
            if (!read_integer(&value)) {
                position_ = start;
                return skip_value(0);
            }
            if (value >= 1 && value <= 65535) c->listen_port = static_cast<int>(value);
            return true;
        }
        bool* field = nullptr;
        if (key == "stackLimitIncrease") field = &c->stack_limit_increase;
        else if (key == "moveMergeEnabled") field = &c->move_merge_enabled;
        else if (key == "opEnabled") field = &c->op_enabled;
        else if (key == "extensionBagEnabled") field = &c->extension_bag_enabled;
        else if (key == "customRecipeEnabled") field = &c->custom_recipe_enabled;
        else if (key == "simpleModeEnabled") field = &c->simple_mode_enabled;
        else if (key == "pushAllEnabled") field = &c->push_all_enabled;
        else if (key == "autoSellEnabled") field = &c->auto_sell_enabled;
        else if (key == "apiEnabled") field = &c->api_enabled;
        else if (key == "debugLogEnabled") field = &c->debug_log_enabled;
        if (field == nullptr) return false;
        const std::size_t start = position_;
        if (literal("true")) { *field = true; return true; }
        position_ = start;
        if (literal("false")) { *field = false; return true; }
        position_ = start;
        return skip_value(0);
    }

    bool skip_value(unsigned depth) {
        if (depth > 64) return false;
        whitespace();
        if (peek('"')) { std::string ignored; return read_string(&ignored); }
        if (consume('{')) {
            whitespace();
            if (consume('}')) return true;
            for (;;) {
                std::string key;
                if (!read_string(&key)) return false;
                whitespace();
                if (!consume(':') || !skip_value(depth + 1)) return false;
                whitespace();
                if (consume('}')) return true;
                if (!consume(',')) return false;
                whitespace();
            }
        }
        if (consume('[')) {
            whitespace();
            if (consume(']')) return true;
            for (;;) {
                if (!skip_value(depth + 1)) return false;
                whitespace();
                if (consume(']')) return true;
                if (!consume(',')) return false;
            }
        }
        if (literal("true") || literal("false") || literal("null")) return true;
        return skip_number();
    }

    bool read_value(JsonValue* value, unsigned depth) {
        if (depth > 64) return false;
        whitespace();
        if (consume('"')) {
            --position_;
            value->kind = JsonValue::Kind::kString;
            return read_string(&value->scalar);
        }
        if (consume('{')) {
            value->kind = JsonValue::Kind::kObject;
            whitespace();
            if (consume('}')) return true;
            for (;;) {
                std::string key;
                if (!read_string(&key)) return false;
                whitespace();
                if (!consume(':')) return false;
                JsonValue member;
                if (!read_value(&member, depth + 1)) return false;
                auto duplicate = std::find_if(value->object.begin(), value->object.end(),
                    [&key](const auto& entry) { return entry.first == key; });
                if (duplicate != value->object.end()) value->object.erase(duplicate);
                value->object.emplace_back(std::move(key), std::move(member));
                whitespace();
                if (consume('}')) return true;
                if (!consume(',')) return false;
                whitespace();
            }
        }
        if (consume('[')) {
            value->kind = JsonValue::Kind::kArray;
            whitespace();
            if (consume(']')) return true;
            for (;;) {
                JsonValue element;
                if (!read_value(&element, depth + 1)) return false;
                value->array.push_back(std::move(element));
                whitespace();
                if (consume(']')) return true;
                if (!consume(',')) return false;
            }
        }
        if (literal("true")) { value->kind = JsonValue::Kind::kBoolean; value->boolean = true; return true; }
        if (literal("false")) { value->kind = JsonValue::Kind::kBoolean; value->boolean = false; return true; }
        if (literal("null")) { value->kind = JsonValue::Kind::kNull; return true; }
        const std::size_t start = position_;
        if (!skip_number()) return false;
        value->kind = JsonValue::Kind::kNumber;
        value->scalar = std::string(input_.substr(start, position_ - start));
        return true;
    }

    static void append_utf8(std::string* out, uint32_t cp) {
        if (cp <= 0x7f) out->push_back(static_cast<char>(cp));
        else if (cp <= 0x7ff) {
            out->push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            out->push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out->push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    bool hex4(uint32_t* result) {
        if (input_.size() - position_ < 4) return false;
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char ch = input_[position_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9') value |= ch - '0';
            else if (ch >= 'a' && ch <= 'f') value |= ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') value |= ch - 'A' + 10;
            else return false;
        }
        *result = value;
        return true;
    }

    bool read_string(std::string* out) {
        if (!consume('"')) return false;
        out->clear();
        while (position_ < input_.size()) {
            const unsigned char ch = static_cast<unsigned char>(input_[position_++]);
            if (ch == '"') return true;
            if (ch < 0x20) return false;
            if (ch >= 0x80) {
                const std::size_t start = position_ - 1;
                std::size_t length = 0;
                if (ch >= 0xc2 && ch <= 0xdf) length = 2;
                else if (ch >= 0xe0 && ch <= 0xef) length = 3;
                else if (ch >= 0xf0 && ch <= 0xf4) length = 4;
                else return false;
                if (input_.size() - start < length ||
                    !valid_utf8(input_.substr(start, length))) return false;
                out->append(input_.data() + start, length);
                position_ = start + length;
                continue;
            }
            if (ch != '\\') { out->push_back(static_cast<char>(ch)); continue; }
            if (position_ >= input_.size()) return false;
            const char escape = input_[position_++];
            switch (escape) {
                case '"': case '\\': case '/': out->push_back(escape); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(&cp)) return false;
                    if (cp >= 0xd800 && cp <= 0xdbff) {
                        if (input_.size() - position_ < 6 || input_[position_] != '\\' ||
                            input_[position_ + 1] != 'u') return false;
                        position_ += 2;
                        uint32_t low = 0;
                        if (!hex4(&low) || low < 0xdc00 || low > 0xdfff) return false;
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
                    append_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool read_integer(int64_t* out) {
        const std::size_t begin = position_;
        if (!skip_number()) return false;
        const std::string_view number = input_.substr(begin, position_ - begin);
        if (number.find_first_of(".eE") != std::string_view::npos) return false;
        try { *out = std::stoll(std::string(number)); }
        catch (...) { return false; }
        return true;
    }

    bool skip_number() {
        const std::size_t start = position_;
        if (peek('-')) ++position_;
        if (peek('0')) ++position_;
        else {
            if (position_ >= input_.size() || input_[position_] < '1' || input_[position_] > '9') return false;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        if (consume('.')) {
            const std::size_t before = position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
            if (before == position_) return false;
        }
        if (consume('e') || consume('E')) {
            if (peek('+') || peek('-')) ++position_;
            const std::size_t before = position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
            if (before == position_) return false;
        }
        return position_ > start;
    }

    bool literal(std::string_view value) {
        if (input_.substr(position_, value.size()) != value) return false;
        position_ += value.size();
        return true;
    }
    void whitespace() { while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\n' || input_[position_] == '\r' || input_[position_] == '\t')) ++position_; }
    bool peek(char ch) const { return position_ < input_.size() && input_[position_] == ch; }
    bool consume(char ch) { if (!peek(ch)) return false; ++position_; return true; }
    std::string_view input_;
    std::size_t position_ = 0;
};

std::string boolean(bool value);
std::string quote(const std::string& value);

std::string serialize_value(const JsonValue& value) {
    switch (value.kind) {
        case JsonValue::Kind::kNull: return "null";
        case JsonValue::Kind::kBoolean: return boolean(value.boolean);
        case JsonValue::Kind::kNumber: return value.scalar;
        case JsonValue::Kind::kString: return quote(value.scalar);
        case JsonValue::Kind::kArray: {
            std::string out = "[";
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) out.push_back(',');
                out += serialize_value(value.array[i]);
            }
            return out + ']';
        }
        case JsonValue::Kind::kObject: {
            std::string out = "{";
            for (std::size_t i = 0; i < value.object.size(); ++i) {
                if (i != 0) out.push_back(',');
                out += quote(value.object[i].first) + ':' + serialize_value(value.object[i].second);
            }
            return out + '}';
        }
    }
    return "null";
}

const JsonValue* member(const JsonValue& object, const std::string& key) {
    if (object.kind != JsonValue::Kind::kObject) return nullptr;
    const auto found = std::find_if(object.object.begin(), object.object.end(),
        [&key](const auto& entry) { return entry.first == key; });
    return found == object.object.end() ? nullptr : &found->second;
}

JsonValue* mutable_member(JsonValue* object, const char* key) {
    if (object == nullptr || object->kind != JsonValue::Kind::kObject) return nullptr;
    const auto found = std::find_if(object->object.begin(), object->object.end(),
        [key](const auto& entry) { return entry.first == key; });
    return found == object->object.end() ? nullptr : &found->second;
}

void set_member(JsonValue* object, const std::string& key, JsonValue value) {
    auto found = std::find_if(object->object.begin(), object->object.end(),
        [&key](const auto& entry) { return entry.first == key; });
    if (found == object->object.end()) object->object.emplace_back(key, std::move(value));
    else found->second = std::move(value);
}

JsonValue boolean_value(bool value) {
    JsonValue result;
    result.kind = JsonValue::Kind::kBoolean;
    result.boolean = value;
    return result;
}

JsonValue string_value(const std::string& value) {
    JsonValue result;
    result.kind = JsonValue::Kind::kString;
    result.scalar = value;
    return result;
}

JsonValue number_value(int value) {
    JsonValue result;
    result.kind = JsonValue::Kind::kNumber;
    result.scalar = std::to_string(value);
    return result;
}

struct LegacyField {
    const char* flat;
    const char* plugin;
    const char* key;
};

const std::array<LegacyField, 12>& legacy_fields() {
    static constexpr std::array<LegacyField, 12> fields{{
        {"listenAddress", "core.settings", "listenAddress"},
        {"listenPort", "core.settings", "listenPort"},
        {"stackLimitIncrease", "stack_limit", "enable"},
        {"moveMergeEnabled", "move_merge", "enable"},
        {"opEnabled", "core.settings", "opEnabled"},
        {"extensionBagEnabled", "extension_bag", "enable"},
        {"customRecipeEnabled", "custom_recipe", "enable"},
        {"simpleModeEnabled", "simple_mode", "enable"},
        {"pushAllEnabled", "push_all", "enable"},
        {"autoSellEnabled", "autosell", "enable"},
        {"apiEnabled", "core.settings", "apiEnabled"},
        {"debugLogEnabled", "core.settings", "debugLogEnabled"},
    }};
    return fields;
}

const std::vector<std::string>& legacy_namespace_ids() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> result{"core.settings"};
        for (const LegacyField& field : legacy_fields()) {
            if (std::find(result.begin(), result.end(), field.plugin) == result.end())
                result.emplace_back(field.plugin);
        }
        std::sort(result.begin(), result.end());
        return result;
    }();
    return ids;
}

bool is_legacy_namespace(const std::string& plugin_id) {
    const auto& ids = legacy_namespace_ids();
    return std::binary_search(ids.begin(), ids.end(), plugin_id);
}

bool legacy_enable_default(const std::string& plugin_id, bool* out) {
    if (plugin_id == "core.settings") {
        *out = true;
        return true;
    }
    if (plugin_id == "extension_bag") {
        *out = true;
        return true;
    }
    for (const LegacyField& field : legacy_fields()) {
        if (plugin_id == field.plugin && std::strcmp(field.key, "enable") == 0) {
            *out = false;
            return true;
        }
    }
    return false;
}

JsonValue default_namespace(const std::string& plugin_id) {
    JsonValue object;
    object.kind = JsonValue::Kind::kObject;
    bool enable = true;
    (void)legacy_enable_default(plugin_id, &enable);
    set_member(&object, "enable", boolean_value(enable));
    if (plugin_id == "core.settings") {
        set_member(&object, "listenAddress", string_value("0.0.0.0"));
        set_member(&object, "listenPort", number_value(8088));
        set_member(&object, "opEnabled", boolean_value(false));
        set_member(&object, "apiEnabled", boolean_value(true));
        set_member(&object, "debugLogEnabled", boolean_value(false));
    }
    return object;
}

bool registered(const std::vector<std::string>& ids, const std::string& id) {
    return std::binary_search(ids.begin(), ids.end(), id);
}

JsonValue* find_namespace(std::vector<std::pair<std::string, JsonValue>>* namespaces,
                          const std::string& id) {
    const auto found = std::find_if(namespaces->begin(), namespaces->end(),
        [&id](const auto& entry) { return entry.first == id; });
    return found == namespaces->end() ? nullptr : &found->second;
}

const JsonValue* find_namespace(const std::vector<std::pair<std::string, JsonValue>>& namespaces,
                                const std::string& id) {
    const auto found = std::find_if(namespaces.begin(), namespaces.end(),
        [&id](const auto& entry) { return entry.first == id; });
    return found == namespaces.end() ? nullptr : &found->second;
}

JsonValue* find_any_namespace(std::vector<std::pair<std::string, JsonValue>>* namespaces,
                              std::vector<std::pair<std::string, JsonValue>>* unknown,
                              const std::string& id) {
    JsonValue* object = find_namespace(namespaces, id);
    return object != nullptr ? object : find_namespace(unknown, id);
}

bool has_feature(const std::vector<PluginFeatureSeed>& definitions, const char* plugin_id,
                 const char* feature_id) {
    for (const auto& plugin : definitions) {
        if (plugin.plugin_id != plugin_id) continue;
        return std::any_of(plugin.features.begin(), plugin.features.end(),
            [feature_id](const contracts::FeatureDefinition& feature) {
                return feature.id != nullptr && std::strcmp(feature.id, feature_id) == 0;
            });
    }
    return false;
}

const PluginFeatureSeed* find_seed(const std::vector<PluginFeatureSeed>& definitions,
                                   const char* plugin_id) {
    const auto found = std::find_if(definitions.begin(), definitions.end(),
        [plugin_id](const PluginFeatureSeed& plugin) { return plugin.plugin_id == plugin_id; });
    return found == definitions.end() ? nullptr : &*found;
}

bool saved_feature_value(const std::vector<std::pair<std::string, JsonValue>>& namespaces,
                         const std::vector<std::pair<std::string, JsonValue>>& unknown,
                         const char* plugin_id, const char* feature_id, bool* out) {
    const JsonValue* object = find_namespace(namespaces, plugin_id);
    if (object == nullptr) object = find_namespace(unknown, plugin_id);
    const JsonValue* features = object == nullptr ? nullptr : member(*object, "features");
    const JsonValue* value = features == nullptr ? nullptr : member(*features, feature_id);
    if (value == nullptr || value->kind != JsonValue::Kind::kBoolean) return false;
    *out = value->boolean;
    return true;
}

bool set_raw_feature(std::vector<std::pair<std::string, JsonValue>>* namespaces,
                     std::vector<std::pair<std::string, JsonValue>>* unknown,
                     const char* plugin_id, const char* feature_id, bool value) {
    JsonValue* object = find_any_namespace(namespaces, unknown, plugin_id);
    if (object == nullptr) {
        JsonValue fresh;
        fresh.kind = JsonValue::Kind::kObject;
        unknown->emplace_back(plugin_id, std::move(fresh));
        object = find_namespace(unknown, plugin_id);
    }
    JsonValue* features = nullptr;
    for (auto& entry : object->object) {
        if (entry.first == "features") {
            if (entry.second.kind != JsonValue::Kind::kObject) return false;
            features = &entry.second;
        }
    }
    if (features == nullptr) {
        JsonValue fresh;
        fresh.kind = JsonValue::Kind::kObject;
        set_member(object, "features", std::move(fresh));
        features = mutable_member(object, "features");
    }
    const JsonValue* current = member(*features, feature_id);
    if (current && current->kind == JsonValue::Kind::kBoolean && current->boolean == value)
        return true;
    set_member(features, feature_id, boolean_value(value));
    return true;
}

bool remove_member(JsonValue* object, const char* key) {
    if (object == nullptr || object->kind != JsonValue::Kind::kObject) return false;
    const auto found = std::find_if(object->object.begin(), object->object.end(),
        [key](const auto& entry) { return entry.first == key; });
    if (found == object->object.end()) return false;
    object->object.erase(found);
    return true;
}

bool remove_namespace_key(std::vector<std::pair<std::string, JsonValue>>* namespaces,
                          std::vector<std::pair<std::string, JsonValue>>* unknown,
                          const char* plugin_id, const char* section, const char* key) {
    JsonValue* object = find_any_namespace(namespaces, unknown, plugin_id);
    JsonValue* values = nullptr;
    if (object != nullptr) {
        for (auto& entry : object->object) {
            if (entry.first == section && entry.second.kind == JsonValue::Kind::kObject) {
                values = &entry.second;
                break;
            }
        }
    }
    return remove_member(values, key);
}

const JsonValue* saved_config_value(
    const std::vector<std::pair<std::string, JsonValue>>& namespaces,
    const std::vector<std::pair<std::string, JsonValue>>& unknown,
    const char* plugin_id, const char* key) {
    const JsonValue* object = find_namespace(namespaces, plugin_id);
    if (object == nullptr) object = find_namespace(unknown, plugin_id);
    const JsonValue* config = object == nullptr ? nullptr : member(*object, "config");
    return config == nullptr ? nullptr : member(*config, key);
}

const JsonValue* saved_root_value(
    const std::vector<std::pair<std::string, JsonValue>>& root, const char* key) {
    const auto found = std::find_if(root.begin(), root.end(),
        [key](const auto& entry) { return entry.first == key; });
    return found == root.end() ? nullptr : &found->second;
}

void remove_root_value(std::vector<std::pair<std::string, JsonValue>>* root, const char* key) {
    const auto found = std::find_if(root->begin(), root->end(),
        [key](const auto& entry) { return entry.first == key; });
    if (found != root->end()) root->erase(found);
}

bool set_raw_config(std::vector<std::pair<std::string, JsonValue>>* namespaces,
                    std::vector<std::pair<std::string, JsonValue>>* unknown,
                    const char* plugin_id, const char* key, const JsonValue& value) {
    JsonValue* object = find_any_namespace(namespaces, unknown, plugin_id);
    if (object == nullptr) {
        JsonValue fresh;
        fresh.kind = JsonValue::Kind::kObject;
        unknown->emplace_back(plugin_id, std::move(fresh));
        object = find_namespace(unknown, plugin_id);
    }
    JsonValue* config = nullptr;
    for (auto& entry : object->object) {
        if (entry.first == "config") {
            if (entry.second.kind != JsonValue::Kind::kObject) return false;
            config = &entry.second;
        }
    }
    if (config == nullptr) {
        JsonValue fresh;
        fresh.kind = JsonValue::Kind::kObject;
        set_member(object, "config", std::move(fresh));
        config = mutable_member(object, "config");
    }
    set_member(config, key, value);
    return true;
}

bool validate_migrated_config(const std::vector<PluginFeatureSeed>& definitions,
                              const char* owner, const char* key, const JsonValue& value) {
    const auto* plugin = find_seed(definitions, owner);
    if (plugin == nullptr || plugin->validate_config == nullptr) return true;
    const std::string json = serialize_value(value);
    return contracts::succeeded(plugin->validate_config(
        key, json.data(), static_cast<uint32_t>(json.size())));
}

bool migrate_host_owner_values(const std::vector<PluginFeatureSeed>& definitions,
                               std::vector<std::pair<std::string, JsonValue>>* namespaces,
                               std::vector<std::pair<std::string, JsonValue>>* unknown) {
    if (has_feature(definitions, "core.api", "api.enabled")) {
        bool target = false;
        if (!saved_feature_value(*namespaces, *unknown, "core.api", "api.enabled", &target)) {
            bool platform_api = true;
            bool registry_api = true;
            const bool has_platform_api = saved_feature_value(
                *namespaces, *unknown, "module.platform", "api.enabled", &platform_api);
            const JsonValue* flat_api = saved_root_value(*unknown, "apiEnabled");
            if (!has_platform_api && flat_api != nullptr &&
                flat_api->kind == JsonValue::Kind::kBoolean) {
                platform_api = flat_api->boolean;
            }
            (void)saved_feature_value(*namespaces, *unknown, "core.api", "core.api.http_registry",
                                      &registry_api);
            if (set_raw_feature(namespaces, unknown, "core.api", "api.enabled",
                                platform_api && registry_api)) {
                remove_namespace_key(namespaces, unknown, "module.platform", "features", "api.enabled");
                remove_namespace_key(namespaces, unknown, "core.api", "features",
                                     "core.api.http_registry");
                remove_root_value(unknown, "apiEnabled");
            }
        } else {
            remove_namespace_key(namespaces, unknown, "module.platform", "features", "api.enabled");
            remove_namespace_key(namespaces, unknown, "core.api", "features",
                                 "core.api.http_registry");
            remove_root_value(unknown, "apiEnabled");
        }

        const struct ConfigMove { const char* old_owner; const char* old_key; const char* new_key; } moves[] = {
            {"module.platform", "listen_address", "listen_address"},
            {"module.platform", "listen_port", "listen_port"},
            {"module.platform", "listenAddress", "listen_address"},
            {"module.platform", "listenPort", "listen_port"},
            {"core.api", "listenAddress", "listen_address"},
            {"core.api", "listenPort", "listen_port"},
            {"core.settings", "listenAddress", "listen_address"},
            {"core.settings", "listenPort", "listen_port"},
        };
        for (const auto& move : moves) {
            const JsonValue* old = saved_config_value(*namespaces, *unknown,
                                                       move.old_owner, move.old_key);
            if (old == nullptr) continue;
            const JsonValue* target = saved_config_value(*namespaces, *unknown,
                                                          "core.api", move.new_key);
            JsonValue old_copy = *old;
            if (target == nullptr) {
                if (!validate_migrated_config(definitions, "core.api", move.new_key, old_copy))
                    return false;
                const bool valid_address = std::strcmp(move.new_key, "listen_address") == 0 &&
                    old->kind == JsonValue::Kind::kString &&
                    old->scalar.find_first_not_of(" \t\r\n\f\v") != std::string::npos;
                bool valid_port = false;
                if (std::strcmp(move.new_key, "listen_port") == 0 &&
                    old->kind == JsonValue::Kind::kNumber) {
                    try {
                        std::size_t used = 0;
                        const auto port = std::stoll(old->scalar, &used);
                        valid_port = used == old->scalar.size() && port >= 1 && port <= 65535;
                    } catch (const std::exception&) {
                        valid_port = false;
                    }
                }
                if ((valid_address || valid_port) &&
                    !set_raw_config(namespaces, unknown, "core.api", move.new_key, old_copy))
                    continue;
            }
            remove_namespace_key(namespaces, unknown, move.old_owner, "config", move.old_key);
        }
        const struct FlatConfigMove { const char* old_key; const char* new_key; } flat_moves[] = {
            {"listenAddress", "listen_address"}, {"listenPort", "listen_port"},
        };
        for (const auto& move : flat_moves) {
            const JsonValue* old = saved_root_value(*unknown, move.old_key);
            if (old == nullptr) continue;
            const JsonValue* target = saved_config_value(*namespaces, *unknown,
                                                          "core.api", move.new_key);
            if (target == nullptr) {
                const JsonValue copy = *old;
                if (!validate_migrated_config(definitions, "core.api", move.new_key, copy))
                    return false;
                const bool address = std::strcmp(move.new_key, "listen_address") == 0 &&
                    old->kind == JsonValue::Kind::kString &&
                    old->scalar.find_first_not_of(" \t\r\n\f\v") != std::string::npos;
                bool port = false;
                if (std::strcmp(move.new_key, "listen_port") == 0 &&
                    old->kind == JsonValue::Kind::kNumber) {
                    try {
                        std::size_t used = 0;
                        const auto parsed = std::stoll(old->scalar, &used);
                        port = used == old->scalar.size() && parsed >= 1 && parsed <= 65535;
                    } catch (const std::exception&) {
                        port = false;
                    }
                }
                if ((address || port) &&
                    !set_raw_config(namespaces, unknown, "core.api", move.new_key, copy))
                    continue;
            }
            remove_root_value(unknown, move.old_key);
        }
    }

    if (has_feature(definitions, "module.security", "op.enabled")) {
        bool target = false;
        if (!saved_feature_value(*namespaces, *unknown, "module.security", "op.enabled", &target)) {
            bool old_value = false;
            bool has_old_value = saved_feature_value(*namespaces, *unknown, "module.platform",
                                                     "op.enabled", &old_value);
            if (!has_old_value) {
                const JsonValue* flat = saved_root_value(*unknown, "opEnabled");
                if (flat != nullptr && flat->kind == JsonValue::Kind::kBoolean) {
                    old_value = flat->boolean;
                    has_old_value = true;
                }
            }
            if (!has_old_value) {
                const JsonValue* legacy = saved_config_value(*namespaces, *unknown,
                                                              "core.settings", "opEnabled");
                if (legacy && legacy->kind == JsonValue::Kind::kBoolean) {
                    old_value = legacy->boolean;
                    has_old_value = true;
                }
            }
            if (has_old_value) (void)set_raw_feature(namespaces, unknown, "module.security",
                                                      "op.enabled", old_value);
        }
        remove_namespace_key(namespaces, unknown, "module.platform", "features", "op.enabled");
        remove_namespace_key(namespaces, unknown, "core.settings", "config", "opEnabled");
        remove_root_value(unknown, "opEnabled");
    }
    return true;
}

bool seed_and_repair_namespace(const std::string& plugin_id,
                               std::vector<std::pair<std::string, JsonValue>>* namespaces) {
    bool changed = false;
    JsonValue* object = find_namespace(namespaces, plugin_id);
    if (object == nullptr || object->kind != JsonValue::Kind::kObject) {
        JsonValue fresh = default_namespace(plugin_id);
        if (object == nullptr) namespaces->emplace_back(plugin_id, std::move(fresh));
        else *object = std::move(fresh);
        object = find_namespace(namespaces, plugin_id);
        changed = true;
    }
    const JsonValue defaults = default_namespace(plugin_id);
    const JsonValue* default_enable = member(defaults, "enable");
    const bool enable_default = default_enable != nullptr && default_enable->kind == JsonValue::Kind::kBoolean &&
                                default_enable->boolean;
    const JsonValue* enable = member(*object, "enable");
    if (enable == nullptr || enable->kind != JsonValue::Kind::kBoolean) {
        set_member(object, "enable", boolean_value(enable_default));
        changed = true;
    }
    if (plugin_id == "core.settings") {
        const JsonValue* address = member(*object, "listenAddress");
        if (address == nullptr || address->kind != JsonValue::Kind::kString ||
            address->scalar.find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
            set_member(object, "listenAddress", string_value("0.0.0.0"));
            changed = true;
        }
        const JsonValue* port = member(*object, "listenPort");
        int64_t parsed_port = 0;
        bool valid_port = port != nullptr && port->kind == JsonValue::Kind::kNumber;
        if (valid_port) {
            try {
                std::size_t used = 0;
                parsed_port = std::stoll(port->scalar, &used);
                valid_port = used == port->scalar.size() && parsed_port >= 1 && parsed_port <= 65535;
            } catch (const std::exception&) {
                valid_port = false;
            }
        }
        if (!valid_port) {
            set_member(object, "listenPort", number_value(8088));
            changed = true;
        }
        const std::pair<const char*, bool> bool_fields[] = {
            {"opEnabled", false}, {"apiEnabled", true}, {"debugLogEnabled", false}};
        for (const auto& field : bool_fields) {
            const JsonValue* value = member(*object, field.first);
            if (value == nullptr || value->kind != JsonValue::Kind::kBoolean) {
                set_member(object, field.first, boolean_value(field.second));
                changed = true;
            }
        }
    }
    return changed;
}

ModuleConfig project_config(const std::vector<std::pair<std::string, JsonValue>>& namespaces,
                            const std::vector<std::string>& registered_plugin_ids) {
    ModuleConfig config;
    if (registered(registered_plugin_ids, "core.settings")) {
        const JsonValue* core = find_namespace(namespaces, "core.settings");
        if (core != nullptr && core->kind == JsonValue::Kind::kObject) {
            if (const JsonValue* value = member(*core, "listenAddress");
                value != nullptr && value->kind == JsonValue::Kind::kString)
                config.listen_address = value->scalar;
            if (const JsonValue* value = member(*core, "listenPort");
                value != nullptr && value->kind == JsonValue::Kind::kNumber) {
                try { config.listen_port = std::stoi(value->scalar); }
                catch (const std::exception&) { config.listen_port = 8088; }
            }
            const auto core_bool = [core](const char* key, bool fallback) {
                const JsonValue* value = member(*core, key);
                return value != nullptr && value->kind == JsonValue::Kind::kBoolean
                           ? value->boolean : fallback;
            };
            config.op_enabled = core_bool("opEnabled", false);
            config.api_enabled = core_bool("apiEnabled", true);
            config.debug_log_enabled = core_bool("debugLogEnabled", false);
        }
    }
    const auto feature_enabled = [&namespaces, &registered_plugin_ids](const char* plugin_id,
                                                                        bool fallback) {
        if (!registered(registered_plugin_ids, plugin_id)) return fallback;
        const JsonValue* object = find_namespace(namespaces, plugin_id);
        const JsonValue* value = object == nullptr ? nullptr : member(*object, "enable");
        return value != nullptr && value->kind == JsonValue::Kind::kBoolean ? value->boolean : fallback;
    };
    config.stack_limit_increase = feature_enabled("stack_limit", false);
    config.move_merge_enabled = feature_enabled("move_merge", false);
    config.extension_bag_enabled = feature_enabled("extension_bag", true);
    config.custom_recipe_enabled = feature_enabled("custom_recipe", false);
    config.simple_mode_enabled = feature_enabled("simple_mode", false);
    config.push_all_enabled = feature_enabled("push_all", false);
    config.auto_sell_enabled = feature_enabled("autosell", false);
    return config;
}

std::string serialize(const std::vector<std::pair<std::string, JsonValue>>& namespaces,
                      const std::vector<std::pair<std::string, JsonValue>>& unknown_root) {
    std::map<std::string, const JsonValue*> sorted_namespaces;
    for (const auto& entry : namespaces) sorted_namespaces[entry.first] = &entry.second;
    std::string out = "{";
    bool first = true;
    const auto append_entry = [&out, &first](const std::string& id, const JsonValue& value) {
        if (!first) out.push_back(',');
        first = false;
        out += quote(id) + ':' + serialize_value(value);
    };
    for (const auto& entry : sorted_namespaces) append_entry(entry.first, *entry.second);
    for (const auto& entry : unknown_root) append_entry(entry.first, entry.second);
    out.push_back('}');
    return out;
}

std::string serialize_registered_namespaces(
    const std::vector<std::pair<std::string, JsonValue>>& namespaces,
    const std::vector<std::string>& registered_plugin_ids) {
    std::vector<std::pair<std::string, JsonValue>> visible;
    visible.reserve(registered_plugin_ids.size());
    for (const std::string& plugin_id : registered_plugin_ids) {
        const JsonValue* object = find_namespace(namespaces, plugin_id);
        if (object != nullptr) visible.emplace_back(plugin_id, *object);
    }
    return serialize(visible, {});
}

bool valid_plugin_id(const std::string& id) {
    if (id.empty() || id.size() > 128 || id[0] < 'a' || id[0] > 'z') return false;
    for (unsigned char ch : id) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '.' || ch == '_' || ch == '-')) return false;
    }
    return true;
}

std::string boolean(bool value) { return value ? "true" : "false"; }

std::string quote(const std::string& value) {
    std::string out = "\"";
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00"; out.push_back(hex[ch >> 4]); out.push_back(hex[ch & 15]);
                } else out.push_back(static_cast<char>(ch));
        }
    }
    return out + '"';
}

bool write_atomic(const std::string& path, const std::string& contents) {
    if (contents.size() > kMaxConfigBytes) return false;
    const std::string temp = path + ".tmp";
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    std::size_t written = 0;
    bool ok = true;
    while (written < contents.size()) {
        const ssize_t count = ::write(fd, contents.data() + written, contents.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        written += static_cast<std::size_t>(count);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (ok && ::rename(temp.c_str(), path.c_str()) == 0) return true;
    ::unlink(temp.c_str());
    return false;
}

ConfigStoreResult fail(ConfigStoreError code, const char* message, bool persisted = false) {
    return {code, message, persisted};
}

}  // namespace

ConfigStoreResult parse_json_document(const std::string& json, JsonValue* out) {
    if (out == nullptr) return fail(ConfigStoreError::kInvalidArgument, "null JSON output");
    if (json.size() > kMaxConfigBytes)
        return fail(ConfigStoreError::kValidation, "JSON document exceeds size limit");
    JsonValue parsed;
    if (!JsonParser(json).read_value_document(&parsed))
        return fail(ConfigStoreError::kValidation, "invalid JSON document");
    *out = std::move(parsed);
    return {};
}

ConfigStore::ConfigStore()
    = default;

std::string serialize_json_value(const JsonValue& value) { return serialize_value(value); }

ConfigStoreResult ConfigStore::prepare_features(const std::vector<PluginFeatureSeed>& definitions) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (init_attempted_) return fail(ConfigStoreError::kValidation, "feature model must precede init");
    std::vector<std::string> ids;
    for (const auto& plugin : definitions) {
        if (!valid_plugin_id(plugin.plugin_id) || plugin.features.empty() ||
            std::find(ids.begin(), ids.end(), plugin.plugin_id) != ids.end())
            return fail(ConfigStoreError::kValidation, "invalid or duplicate plugin/zero features");
        ids.push_back(plugin.plugin_id);
        std::vector<std::string> feature_ids;
        for (const auto& feature : plugin.features) {
            if (feature.display_priority < contracts::kMinFeatureDisplayPriority ||
                feature.display_priority > contracts::kMaxFeatureDisplayPriority)
                return fail(ConfigStoreError::kValidation,
                            "feature display priority out of range");
            if (feature.id == nullptr || feature.id[0] == '\0' ||
                std::strlen(feature.id) > 128 || !valid_utf8(feature.id) ||
                feature.default_enabled > 1 || feature.name == nullptr ||
                feature.description == nullptr || !valid_utf8(feature.name) ||
                !valid_utf8(feature.description) ||
                static_cast<uint8_t>(feature.category) > static_cast<uint8_t>(contracts::FeatureCategory::kBasicOptimization) ||
                (feature.type != contracts::FeatureType::kToggle && feature.type != contracts::FeatureType::kEntry) ||
                (feature.type == contracts::FeatureType::kToggle && (feature.on_toggle == nullptr || feature.on_entry != nullptr)) ||
                (feature.type == contracts::FeatureType::kEntry && (feature.on_entry == nullptr || feature.on_toggle != nullptr)) ||
                std::find(feature_ids.begin(), feature_ids.end(), feature.id) != feature_ids.end())
                return fail(ConfigStoreError::kValidation, "invalid or duplicate feature");
            feature_ids.emplace_back(feature.id);
        }
    }
    feature_definitions_ = definitions;
    feature_model_ = true;
    return {};
}

ConfigStoreSeedResult ConfigStore::seed_features() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!feature_model_ || !initialized_)
        return {fail(ConfigStoreError::kNotReady, "feature model not initialized")};
    // Validate the entire persisted batch before constructing defaults or a commit candidate.
    // Defaults fill absent values only; an existing value with the wrong type is an error.
    for (const auto& plugin : feature_definitions_) {
        const JsonValue* object = find_namespace(namespaces_, plugin.plugin_id);
        if (!object) object = find_namespace(unknown_root_, plugin.plugin_id);
        if (!object) continue;
        if (object->kind != JsonValue::Kind::kObject)
            return {fail(ConfigStoreError::kValidation, "plugin namespace must be an object")};
        const auto* enable = member(*object, "enable");
        if (enable && enable->kind != JsonValue::Kind::kBoolean)
            return {fail(ConfigStoreError::kValidation, "plugin enable must be boolean")};
        const auto* features = member(*object, "features");
        if (features && features->kind != JsonValue::Kind::kObject)
            return {fail(ConfigStoreError::kValidation, "plugin features must be an object")};
        const auto* config = member(*object, "config");
        if (config && config->kind != JsonValue::Kind::kObject)
            return {fail(ConfigStoreError::kValidation, "plugin config must be an object")};
        if (features) for (const auto& feature : plugin.features) {
            const auto* value = member(*features, feature.id);
            if (value && value->kind != JsonValue::Kind::kBoolean)
                return {fail(ConfigStoreError::kValidation, "registered feature must be boolean")};
        }
    }
    auto next = namespaces_;
    auto unknown = unknown_root_;
    const std::string before_migration = serialize(next, unknown);
    if (!migrate_host_owner_values(feature_definitions_, &next, &unknown))
        return {fail(ConfigStoreError::kValidation, "migrated plugin config validation failed")};
    const bool migration_changed = serialize(next, unknown) != before_migration;
    std::vector<std::string> ids;
    for (const auto& plugin : feature_definitions_) {
        ids.push_back(plugin.plugin_id);
        JsonValue* object = find_namespace(&next, plugin.plugin_id);
        if (object == nullptr) {
            const auto found = std::find_if(unknown.begin(), unknown.end(), [&](const auto& item) {
                return item.first == plugin.plugin_id;
            });
            if (found != unknown.end()) {
                next.push_back(*found);
                unknown.erase(found);
            } else {
                JsonValue fresh;
                fresh.kind = JsonValue::Kind::kObject;
                next.emplace_back(plugin.plugin_id, fresh);
            }
            object = find_namespace(&next, plugin.plugin_id);
        }
        if (plugin.validate_config != nullptr) {
            const JsonValue* config = member(*object, "config");
            if (config != nullptr && config->kind == JsonValue::Kind::kObject) {
                for (const auto& entry : config->object) {
                    const std::string json = serialize_value(entry.second);
                    const QolResult validation = plugin.validate_config(
                        entry.first.c_str(), json.data(), static_cast<uint32_t>(json.size()));
                    if (!contracts::succeeded(validation))
                        return {fail(ConfigStoreError::kValidation,
                                     "plugin config validation failed")};
                }
            }
        }
        JsonValue schema;
        schema.kind = JsonValue::Kind::kObject;
        const auto* enable = member(*object, "enable");
        set_member(&schema, "enable", enable ? *enable : boolean_value(true));
        JsonValue features;
        features.kind = JsonValue::Kind::kObject;
        if (const auto* old = member(*object, "features"))
            features = *old;
        for (const auto& feature : plugin.features) {
            const auto* value = member(features, feature.id);
            if (!value)
                set_member(&features, feature.id, boolean_value(feature.default_enabled != 0));
        }
        set_member(&schema, "features", std::move(features));
        JsonValue config;
        config.kind = JsonValue::Kind::kObject;
        if (const auto* old = member(*object, "config"))
            config = *old;
        set_member(&schema, "config", std::move(config));
        *object = std::move(schema);
    }
    std::sort(ids.begin(), ids.end());
    const bool changed = ids != registered_plugin_ids_ || serialize(next, unknown) != serialize(namespaces_, unknown_root_);
    if (!changed && !writeback_pending_) return {{}, false, true, false};
    if (degraded_read_only_ || !write_atomic(path_, serialize(next, unknown))) {
        if (migration_changed)
            return {fail(ConfigStoreError::kPersistence, "feature migration persistence failed"),
                    changed, false, false};
        // Defaults remain readable after a failed bootstrap, but no durable revision/effect is emitted.
        namespaces_ = std::move(next);
        unknown_root_ = std::move(unknown);
        registered_plugin_ids_ = std::move(ids);
        writeback_pending_ = true;
        return {fail(ConfigStoreError::kPersistence, "feature seed persistence failed"), changed, true, false};
    }
    namespaces_ = std::move(next);
    unknown_root_ = std::move(unknown);
    registered_plugin_ids_ = std::move(ids);
    writeback_pending_ = false;
    return {{}, changed, true, true};
}

ConfigStoreResult ConfigStore::read_feature_namespace(const std::string& id, JsonValue* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!feature_model_ || !initialized_) return fail(ConfigStoreError::kNotReady, "feature model not ready");
    if (!out || !registered(registered_plugin_ids_, id)) return fail(ConfigStoreError::kInvalidArgument, "unregistered plugin");
    const auto* object = find_namespace(namespaces_, id);
    if (!object) return fail(ConfigStoreError::kNotReady, "feature defaults not seeded");
    out->kind = JsonValue::Kind::kObject;
    out->object.clear();
    for (const char* key : {"features", "config"}) {
        if (const auto* value = member(*object, key)) set_member(out, key, *value);
    }
    return {ConfigStoreError::kNone, {}, !writeback_pending_ && !degraded_read_only_};
}

ConfigStoreResult ConfigStore::desired_snapshots(std::vector<PluginDesiredSnapshot>* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!out || !feature_model_ || !initialized_) return fail(ConfigStoreError::kNotReady, "feature model not ready");
    out->clear();
    for (const auto& plugin : feature_definitions_) {
        const auto* object = find_namespace(namespaces_, plugin.plugin_id);
        if (!object) return fail(ConfigStoreError::kNotReady, "feature defaults not seeded");
        const auto* enable = member(*object, "enable");
        const auto* features = member(*object, "features");
        PluginDesiredSnapshot snapshot{plugin.plugin_id, enable && enable->kind == JsonValue::Kind::kBoolean && enable->boolean, false};
        if (features) for (const auto& feature : plugin.features) {
            const auto* value = member(*features, feature.id);
            snapshot.any_feature |= value && value->kind == JsonValue::Kind::kBoolean && value->boolean;
        }
        out->push_back(std::move(snapshot));
    }
    return {};
}

ConfigStoreResult ConfigStore::update_feature_model(const std::string& id, FeatureConfigField field,
                                                     const std::string& key, const std::string& json) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!feature_model_ || !initialized_) return fail(ConfigStoreError::kNotReady, "feature model not ready");
    if (degraded_read_only_) return fail(ConfigStoreError::kPersistence, "configuration storage unavailable");
    const auto plugin = std::find_if(feature_definitions_.begin(), feature_definitions_.end(), [&](const auto& item) { return item.plugin_id == id; });
    if (plugin == feature_definitions_.end()) return fail(ConfigStoreError::kValidation, "unregistered plugin");
    JsonValue value;
    const auto parsed = parse_json_document(json, &value);
    if (!parsed.ok()) return parsed;
    if (field != FeatureConfigField::kConfig && value.kind != JsonValue::Kind::kBoolean)
        return fail(ConfigStoreError::kValidation, "feature/enable value must be boolean");
    if (field == FeatureConfigField::kFeature && std::none_of(plugin->features.begin(), plugin->features.end(), [&](const auto& item) { return key == item.id; }))
        return fail(ConfigStoreError::kValidation, "unregistered feature");
    if (field != FeatureConfigField::kDesiredEnable && (!valid_utf8(key) || key.size() > kMaxConfigBytes))
        return fail(ConfigStoreError::kValidation, "invalid config key");
    if (field == FeatureConfigField::kConfig && plugin->validate_config != nullptr) {
        const QolResult validation = plugin->validate_config(
            key.c_str(), json.data(), static_cast<uint32_t>(json.size()));
        if (!contracts::succeeded(validation))
            return fail(ConfigStoreError::kValidation, "plugin config validation failed");
    }
    auto next = namespaces_;
    auto* object = find_namespace(&next, id);
    if (!object) return fail(ConfigStoreError::kNotReady, "feature defaults not seeded");
    if (field == FeatureConfigField::kDesiredEnable) set_member(object, "enable", std::move(value));
    else {
        const std::string section = field == FeatureConfigField::kFeature ? "features" : "config";
        const auto* current = member(*object, section);
        if (!current || current->kind != JsonValue::Kind::kObject) return fail(ConfigStoreError::kValidation, "invalid schema section");
        JsonValue updated = *current;
        set_member(&updated, key, std::move(value));
        set_member(object, section, std::move(updated));
        if (field == FeatureConfigField::kFeature) {
            const auto* features = member(*object, "features");
            bool any_enabled = false;
            for (const auto& feature : plugin->features) {
                const auto* enabled = features ? member(*features, feature.id) : nullptr;
                if (!enabled || enabled->kind != JsonValue::Kind::kBoolean)
                    return fail(ConfigStoreError::kValidation, "registered feature must be boolean");
                any_enabled = any_enabled || enabled->boolean;
            }
            set_member(object, "enable", boolean_value(any_enabled));
        }
    }
    if (!write_atomic(path_, serialize(next, unknown_root_))) return fail(ConfigStoreError::kPersistence, "configuration persistence failed");
    namespaces_ = std::move(next);
    writeback_pending_ = false;
    return {};
}

ConfigStoreSeedResult ConfigStore::seed_registered_plugins(const std::vector<std::string>& requested_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (feature_model_) return {fail(ConfigStoreError::kValidation, "legacy seed unavailable in feature model")};
    if (!initialized_ && !degraded_read_only_)
        return {fail(ConfigStoreError::kNotReady, "configuration store not initialized"), false, false};

    std::vector<std::string> ids = requested_ids;
    std::sort(ids.begin(), ids.end());
    if (std::any_of(ids.begin(), ids.end(), [](const std::string& id) { return !valid_plugin_id(id); }))
        return {fail(ConfigStoreError::kInvalidArgument, "invalid plugin id in seed batch"), false, false};
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    const auto default_enable = [](const std::string& id) {
        const JsonValue built_in = default_namespace(id);
        const JsonValue* enable = member(built_in, "enable");
        return enable != nullptr && enable->kind == JsonValue::Kind::kBoolean && enable->boolean;
    };

    for (const std::string& id : ids) {
        const auto unknown = std::find_if(unknown_root_.begin(), unknown_root_.end(),
            [&id](const auto& entry) { return entry.first == id; });
        if (unknown != unknown_root_.end() && unknown->second.kind != JsonValue::Kind::kObject) {
            return {ConfigStoreResult{ConfigStoreError::kInvalidArgument,
                                      "plugin id conflicts with non-object root value: " + id, false},
                    false, false};
        }
    }
    auto next_namespaces = namespaces_;
    auto next_unknown = unknown_root_;
    bool changed = ids != registered_plugin_ids_;
    for (const std::string& id : ids) {
        JsonValue* object = find_namespace(&next_namespaces, id);
        if (object == nullptr) {
            const auto unknown = std::find_if(next_unknown.begin(), next_unknown.end(),
                [&id](const auto& entry) { return entry.first == id; });
            if (unknown != next_unknown.end()) {
                next_namespaces.emplace_back(id, std::move(unknown->second));
                next_unknown.erase(unknown);
            } else {
                next_namespaces.emplace_back(id, default_namespace(id));
            }
            object = find_namespace(&next_namespaces, id);
            changed = true;
        }
        if (object->kind != JsonValue::Kind::kObject) {
            *object = default_namespace(id);
            changed = true;
        }
        const JsonValue* enable = member(*object, "enable");
        if (enable == nullptr || enable->kind != JsonValue::Kind::kBoolean) {
            set_member(object, "enable", boolean_value(default_enable(id)));
            changed = true;
        }
    }

    const ModuleConfig next_config = project_config(next_namespaces, ids);
    if (degraded_read_only_) {
        registered_plugin_ids_ = std::move(ids);
        namespaces_ = std::move(next_namespaces);
        unknown_root_ = std::move(next_unknown);
        config_ = next_config;
        return {fail(ConfigStoreError::kPersistence, "configuration storage unavailable"),
                changed, true};
    }

    if (!changed && !writeback_pending_) return {{}, false, true, false};
    const bool durable_commit = changed || writeback_pending_;
    const std::string canonical = serialize(next_namespaces, next_unknown);
    if (!write_atomic(path_, canonical)) {
        registered_plugin_ids_ = std::move(ids);
        namespaces_ = std::move(next_namespaces);
        unknown_root_ = std::move(next_unknown);
        config_ = next_config;
        writeback_pending_ = true;
        return {fail(ConfigStoreError::kPersistence, "configuration persistence failed"),
                changed, true};
    }
    registered_plugin_ids_ = std::move(ids);
    namespaces_ = std::move(next_namespaces);
    unknown_root_ = std::move(next_unknown);
    config_ = next_config;
    writeback_pending_ = false;
    return {{}, changed, true, durable_commit};
}

ConfigStoreResult ConfigStore::init(const std::string& directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (init_attempted_) {
        if (directory != init_directory_)
            return fail(ConfigStoreError::kInvalidArgument, "configuration directory already initialized");
        return init_result_;
    }
    init_attempted_ = true;
    init_directory_ = directory;
    config_ = ModuleConfig{};
    namespaces_.clear();
    unknown_root_.clear();
    const auto hard_failure = [this](const char* message) {
        degraded_read_only_ = true;
        for (const std::string& plugin_id : feature_model_ ? std::vector<std::string>{} : legacy_namespace_ids()) {
            JsonValue* object = find_namespace(&namespaces_, plugin_id);
            if (object == nullptr) {
                namespaces_.emplace_back(plugin_id, default_namespace(plugin_id));
            } else {
                (void)seed_and_repair_namespace(plugin_id, &namespaces_);
            }
        }
        config_ = project_config(namespaces_, registered_plugin_ids_);
        initialized_ = true;
        init_result_ = fail(ConfigStoreError::kPersistence, message);
        return init_result_;
    };
    if (directory.empty()) return hard_failure("configuration directory unavailable");
    path_ = directory + (directory.back() == '/' ? "" : "/") + "config.json";
    std::string parent = directory;
    std::size_t position = 1;
    while (position < parent.size()) {
        position = parent.find('/', position);
        if (position == std::string::npos) position = parent.size();
        const std::string current = parent.substr(0, position);
        if (!current.empty() && ::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
            return hard_failure("configuration directory creation failed");
        }
        struct stat directory_info {};
        if (!current.empty() && (::stat(current.c_str(), &directory_info) != 0 || !S_ISDIR(directory_info.st_mode))) {
            return hard_failure("configuration directory creation failed");
        }
        ++position;
    }
    FILE* file = std::fopen(path_.c_str(), "rb");
    if (file != nullptr) {
        struct stat info {};
        std::string bytes;
        if (::fstat(::fileno(file), &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0 || static_cast<std::size_t>(info.st_size) > kMaxConfigBytes) {
            std::fclose(file);
            return hard_failure("configuration read failed");
        }
        bool read_ok = true;
        {
            bytes.resize(static_cast<std::size_t>(info.st_size));
            if (!bytes.empty() && std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) read_ok = false;
        }
        if (std::ferror(file) != 0) read_ok = false;
        if (std::fclose(file) != 0) read_ok = false;
        if (!read_ok) {
            return hard_failure("configuration read failed");
        }
        JsonValue root;
        const bool valid = JsonParser(bytes).read_value_document(&root) &&
                           root.kind == JsonValue::Kind::kObject;
        if (!valid) {
            for (const std::string& plugin_id : feature_model_ ? std::vector<std::string>{} : legacy_namespace_ids())
                namespaces_.emplace_back(plugin_id, default_namespace(plugin_id));
            config_ = project_config(namespaces_, registered_plugin_ids_);
            initialized_ = true;
            if (!write_atomic(path_, serialize(namespaces_, unknown_root_))) {
                writeback_pending_ = true;
                init_result_ = fail(ConfigStoreError::kPersistence,
                                    "configuration persistence failed", false);
                return init_result_;
            }
            writeback_pending_ = false;
            init_result_ = {ConfigStoreError::kNone, "configuration reset", true};
            return init_result_;
        }

        if (feature_model_) {
            unknown_root_ = std::move(root.object);
            initialized_ = true;
            init_result_ = {};
            return init_result_;
        }
        const auto& fields = legacy_fields();
        std::vector<std::pair<std::string, JsonValue>> legacy_values;
        bool needs_migration = false;
        for (const auto& entry : root.object) {
            const auto legacy = std::find_if(fields.begin(), fields.end(),
                [&entry](const LegacyField& field) { return entry.first == field.flat; });
            if (legacy != fields.end()) {
                legacy_values.push_back(entry);
                needs_migration = true;
                continue;
            }
            if (entry.first == "jewelBatchMix") {
                needs_migration = true;
                continue;
            }
            if (is_legacy_namespace(entry.first) && entry.second.kind == JsonValue::Kind::kObject) {
                namespaces_.emplace_back(entry.first, entry.second);
            } else {
                unknown_root_.push_back(entry);
            }
        }

        for (const auto& entry : legacy_values) {
            const auto mapping = std::find_if(fields.begin(), fields.end(),
                [&entry](const LegacyField& field) { return entry.first == field.flat; });
            if (mapping == fields.end()) continue;
            const auto collision = std::find_if(unknown_root_.begin(), unknown_root_.end(),
                [&mapping](const auto& item) { return item.first == mapping->plugin; });
            if (collision != unknown_root_.end() && collision->second.kind != JsonValue::Kind::kObject)
                continue;
            JsonValue* target = find_namespace(&namespaces_, mapping->plugin);
            if (target == nullptr || target->kind != JsonValue::Kind::kObject) {
                JsonValue fresh;
                fresh.kind = JsonValue::Kind::kObject;
                if (target == nullptr) namespaces_.emplace_back(mapping->plugin, std::move(fresh));
                else *target = std::move(fresh);
                target = find_namespace(&namespaces_, mapping->plugin);
            }
            if (member(*target, mapping->key) != nullptr) continue;
            const JsonValue& legacy = entry.second;
            if (std::strcmp(mapping->key, "enable") == 0 ||
                std::strcmp(mapping->key, "opEnabled") == 0 ||
                std::strcmp(mapping->key, "apiEnabled") == 0 ||
                std::strcmp(mapping->key, "debugLogEnabled") == 0) {
                if (legacy.kind == JsonValue::Kind::kBoolean)
                    set_member(target, mapping->key, legacy);
            } else if (std::strcmp(mapping->key, "listenAddress") == 0) {
                if (legacy.kind == JsonValue::Kind::kString) set_member(target, mapping->key, legacy);
            } else if (std::strcmp(mapping->key, "listenPort") == 0 &&
                       legacy.kind == JsonValue::Kind::kNumber) {
                int64_t port = 0;
                try {
                    std::size_t used = 0;
                    port = std::stoll(legacy.scalar, &used);
                    if (used == legacy.scalar.size() && port >= 1 && port <= 65535)
                        set_member(target, mapping->key, legacy);
                } catch (const std::exception&) {
                    continue;
                }
            }
        }

        for (const std::string& plugin_id : legacy_namespace_ids()) {
            const auto collision = std::find_if(unknown_root_.begin(), unknown_root_.end(),
                [&plugin_id](const auto& item) { return item.first == plugin_id; });
            if (collision != unknown_root_.end() && collision->second.kind != JsonValue::Kind::kObject)
                continue;
            needs_migration = seed_and_repair_namespace(plugin_id, &namespaces_) || needs_migration;
        }
        config_ = project_config(namespaces_, registered_plugin_ids_);
        initialized_ = true;
        const std::string canonical = serialize(namespaces_, unknown_root_);
        needs_migration = needs_migration || canonical != bytes;
        if (needs_migration && !write_atomic(path_, canonical)) {
            writeback_pending_ = true;
            init_result_ = fail(ConfigStoreError::kPersistence,
                                "configuration migration persistence failed", false);
            return init_result_;
        }
        writeback_pending_ = false;
        init_result_ = {};
        return init_result_;
    }
    if (errno != ENOENT) {
        return hard_failure("configuration open failed");
    }
    for (const std::string& plugin_id : feature_model_ ? std::vector<std::string>{} : legacy_namespace_ids())
        (void)seed_and_repair_namespace(plugin_id, &namespaces_);
    config_ = project_config(namespaces_, registered_plugin_ids_);
    if (!write_atomic(path_, serialize(namespaces_, unknown_root_))) {
        init_result_ = fail(ConfigStoreError::kPersistence, "configuration persistence failed", false);
        initialized_ = true;
        writeback_pending_ = true;
        return init_result_;
    }
    initialized_ = true;
    writeback_pending_ = false;
    init_result_ = {};
    return init_result_;
}

ModuleConfig ConfigStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

bool ConfigStore::degraded_read_only() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return degraded_read_only_;
}

ConfigStoreResult ConfigStore::update(const ModuleConfigPatch& requested) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (feature_model_) return fail(ConfigStoreError::kValidation, "legacy update unavailable in feature model");
    if (degraded_read_only_) return fail(ConfigStoreError::kPersistence, "configuration storage unavailable");
    if (!initialized_) return fail(ConfigStoreError::kInvalidArgument, "configuration not initialized");
    ModuleConfig next = config_;
    if (requested.listen_address) next.listen_address = *requested.listen_address;
    if (requested.listen_port) next.listen_port = *requested.listen_port;
    if (requested.stack_limit_increase) next.stack_limit_increase = *requested.stack_limit_increase;
    if (requested.move_merge_enabled) next.move_merge_enabled = *requested.move_merge_enabled;
    if (requested.op_enabled) next.op_enabled = *requested.op_enabled;
    if (requested.extension_bag_enabled) next.extension_bag_enabled = *requested.extension_bag_enabled;
    if (requested.custom_recipe_enabled) next.custom_recipe_enabled = *requested.custom_recipe_enabled;
    if (requested.simple_mode_enabled) next.simple_mode_enabled = *requested.simple_mode_enabled;
    if (requested.push_all_enabled) next.push_all_enabled = *requested.push_all_enabled;
    if (requested.auto_sell_enabled) next.auto_sell_enabled = *requested.auto_sell_enabled;
    if (requested.api_enabled) next.api_enabled = *requested.api_enabled;
    if (requested.debug_log_enabled) next.debug_log_enabled = *requested.debug_log_enabled;
    if (next.listen_address.find_first_not_of(" \t\r\n\f\v") == std::string::npos)
        return fail(ConfigStoreError::kValidation, "listenAddress required");
    if (!valid_utf8(next.listen_address))
        return fail(ConfigStoreError::kValidation, "listenAddress must be valid UTF-8");
    if (next.listen_port < 1 || next.listen_port > 65535)
        return fail(ConfigStoreError::kValidation, "listenPort must be 1-65535");
    auto next_namespaces = namespaces_;
    if (registered(registered_plugin_ids_, "core.settings")) {
        JsonValue* core = find_namespace(&next_namespaces, "core.settings");
        if (core == nullptr) return fail(ConfigStoreError::kValidation, "core.settings namespace missing");
        if (requested.listen_address) set_member(core, "listenAddress", string_value(next.listen_address));
        if (requested.listen_port) set_member(core, "listenPort", number_value(next.listen_port));
        if (requested.op_enabled) set_member(core, "opEnabled", boolean_value(next.op_enabled));
        if (requested.api_enabled) set_member(core, "apiEnabled", boolean_value(next.api_enabled));
        if (requested.debug_log_enabled)
            set_member(core, "debugLogEnabled", boolean_value(next.debug_log_enabled));
    }
    const auto set_enabled = [this, &next_namespaces](const char* plugin_id,
                                                       const std::optional<bool>& requested_value,
                                                       bool enabled) {
        if (!requested_value || !registered(registered_plugin_ids_, plugin_id)) return;
        JsonValue* object = find_namespace(&next_namespaces, plugin_id);
        if (object != nullptr) set_member(object, "enable", boolean_value(enabled));
    };
    set_enabled("stack_limit", requested.stack_limit_increase, next.stack_limit_increase);
    set_enabled("move_merge", requested.move_merge_enabled, next.move_merge_enabled);
    set_enabled("extension_bag", requested.extension_bag_enabled, next.extension_bag_enabled);
    set_enabled("custom_recipe", requested.custom_recipe_enabled, next.custom_recipe_enabled);
    set_enabled("simple_mode", requested.simple_mode_enabled, next.simple_mode_enabled);
    set_enabled("push_all", requested.push_all_enabled, next.push_all_enabled);
    set_enabled("autosell", requested.auto_sell_enabled, next.auto_sell_enabled);
    if (!write_atomic(path_, serialize(next_namespaces, unknown_root_)))
        return fail(ConfigStoreError::kPersistence, "configuration persistence failed");
    namespaces_ = std::move(next_namespaces);
    config_ = project_config(namespaces_, registered_plugin_ids_);
    writeback_pending_ = false;
    return {};
}

ConfigStoreResult ConfigStore::read_namespace(const std::string& plugin_id, JsonValue* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (out == nullptr || !valid_plugin_id(plugin_id))
        return fail(ConfigStoreError::kInvalidArgument, "invalid plugin namespace");
    if (!registered(registered_plugin_ids_, plugin_id)) {
        if (degraded_read_only_)
            return fail(ConfigStoreError::kNotReady,
                        "unregistered namespace unavailable while storage is degraded");
        if (!initialized_) return fail(ConfigStoreError::kInvalidArgument, "configuration not initialized");
        return fail(ConfigStoreError::kInvalidArgument, "unregistered plugin namespace");
    }
    const JsonValue* object = find_namespace(namespaces_, plugin_id);
    if (object == nullptr) {
        if (degraded_read_only_)
            return fail(ConfigStoreError::kNotReady, "registered namespace defaults unavailable");
        if (!initialized_) return fail(ConfigStoreError::kInvalidArgument, "configuration not initialized");
        return fail(ConfigStoreError::kNotFound, "namespace not found");
    }
    *out = *object;
    if (feature_model_) out->object.erase(std::remove_if(out->object.begin(), out->object.end(), [](const auto& item) { return item.first == "enable"; }), out->object.end());
    if (degraded_read_only_)
        return {ConfigStoreError::kNone, "degraded in-memory defaults", false};
    if (!initialized_) return fail(ConfigStoreError::kInvalidArgument, "configuration not initialized");
    return {};
}

ConfigStoreResult ConfigStore::read_registered_json(char* out_json, uint32_t capacity,
                                                     uint32_t* out_size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (feature_model_) return fail(ConfigStoreError::kValidation, "legacy registered projection unavailable in feature model");
    if (out_size != nullptr) *out_size = 0;
    if (out_size == nullptr || (capacity != 0 && out_json == nullptr))
        return fail(ConfigStoreError::kInvalidArgument, "invalid registered config buffer");
    if (!initialized_)
        return fail(ConfigStoreError::kNotReady, "configuration store not initialized");
    const std::string json = serialize_registered_namespaces(namespaces_, registered_plugin_ids_);
    if (json.size() > capacity)
        return fail(ConfigStoreError::kValidation, "registered config output buffer too small");
    if (!json.empty()) std::memcpy(out_json, json.data(), json.size());
    *out_size = static_cast<uint32_t>(json.size());
    if (degraded_read_only_)
        return {ConfigStoreError::kNone, "degraded in-memory defaults", false};
    return {};
}

ConfigStoreResult ConfigStore::update_namespace_key(const std::string& caller_plugin_id,
                                                    const std::string& target_plugin_id,
                                                    const std::string& key,
                                                    const std::string& json_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (feature_model_) return fail(ConfigStoreError::kValidation, "legacy update unavailable in feature model");
    if (degraded_read_only_) return fail(ConfigStoreError::kPersistence, "configuration storage unavailable");
    if (!initialized_) return fail(ConfigStoreError::kInvalidArgument, "configuration not initialized");
    if (!valid_plugin_id(caller_plugin_id) || !valid_plugin_id(target_plugin_id) ||
        !registered(registered_plugin_ids_, caller_plugin_id) ||
        !registered(registered_plugin_ids_, target_plugin_id))
        return fail(ConfigStoreError::kInvalidArgument, "unregistered plugin namespace");
    if (key.empty() || key.size() > 128 || !valid_utf8(key))
        return fail(ConfigStoreError::kInvalidArgument, "invalid plugin config key");
    if (json_value.size() > kMaxConfigBytes)
        return fail(ConfigStoreError::kValidation, "config value exceeds size limit");
    JsonValue value;
    if (!JsonParser(json_value).read_value_document(&value))
        return fail(ConfigStoreError::kValidation, "invalid config value JSON");
    auto next_namespaces = namespaces_;
    JsonValue* target = find_namespace(&next_namespaces, target_plugin_id);
    if (target == nullptr || target->kind != JsonValue::Kind::kObject)
        return fail(ConfigStoreError::kNotFound, "target namespace not found");
    const JsonValue* previous = member(*target, key);
    if (caller_plugin_id != target_plugin_id && previous == nullptr)
        return fail(ConfigStoreError::kNotFound, "cross-plugin key does not exist");
    if (key == "enable" && value.kind != JsonValue::Kind::kBoolean)
        return fail(ConfigStoreError::kValidation, "plugin enable must be boolean");
    set_member(target, key, std::move(value));
    const ModuleConfig next_config = project_config(next_namespaces, registered_plugin_ids_);
    if (!write_atomic(path_, serialize(next_namespaces, unknown_root_)))
        return fail(ConfigStoreError::kPersistence, "configuration persistence failed");
    namespaces_ = std::move(next_namespaces);
    config_ = next_config;
    writeback_pending_ = false;
    return {};
}

ConfigStoreResult ConfigStore::replace_namespace(const std::string& plugin_id,
                                                 const std::string& json_value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (feature_model_) return fail(ConfigStoreError::kValidation, "legacy replace unavailable in feature model");
    if (degraded_read_only_) return fail(ConfigStoreError::kPersistence, "configuration storage unavailable");
    if (!initialized_) return fail(ConfigStoreError::kInvalidArgument, "configuration not initialized");
    if (!valid_plugin_id(plugin_id) || !registered(registered_plugin_ids_, plugin_id))
        return fail(ConfigStoreError::kInvalidArgument, "invalid plugin namespace");
    if (json_value.size() > kMaxConfigBytes)
        return fail(ConfigStoreError::kValidation, "namespace JSON exceeds size limit");
    JsonValue value;
    if (!JsonParser(json_value).read_value_document(&value) || value.kind != JsonValue::Kind::kObject)
        return fail(ConfigStoreError::kValidation, "namespace must be a JSON object");
    const JsonValue* enable = member(value, "enable");
    if (enable == nullptr || enable->kind != JsonValue::Kind::kBoolean)
        return fail(ConfigStoreError::kValidation, "plugin enable must be boolean");
    auto next_namespaces = namespaces_;
    JsonValue* existing = find_namespace(&next_namespaces, plugin_id);
    if (existing == nullptr) return fail(ConfigStoreError::kNotFound, "namespace not found");
    *existing = std::move(value);
    (void)seed_and_repair_namespace(plugin_id, &next_namespaces);
    const ModuleConfig next_config = project_config(next_namespaces, registered_plugin_ids_);
    if (!write_atomic(path_, serialize(next_namespaces, unknown_root_)))
        return fail(ConfigStoreError::kPersistence, "configuration persistence failed");
    namespaces_ = std::move(next_namespaces);
    config_ = next_config;
    writeback_pending_ = false;
    return {};
}

ConfigStoreResult ConfigStore::clear_namespace(const std::string& plugin_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (feature_model_) return fail(ConfigStoreError::kValidation, "legacy clear unavailable in feature model");
    if (degraded_read_only_) return fail(ConfigStoreError::kPersistence, "configuration storage unavailable");
    if (!initialized_) return fail(ConfigStoreError::kInvalidArgument, "configuration not initialized");
    if (!valid_plugin_id(plugin_id) || !registered(registered_plugin_ids_, plugin_id))
        return fail(ConfigStoreError::kInvalidArgument, "invalid plugin namespace");
    auto next_namespaces = namespaces_;
    JsonValue* existing = find_namespace(&next_namespaces, plugin_id);
    if (existing == nullptr) return fail(ConfigStoreError::kNotFound, "namespace not found");
    *existing = default_namespace(plugin_id);
    const ModuleConfig next_config = project_config(next_namespaces, registered_plugin_ids_);
    if (!write_atomic(path_, serialize(next_namespaces, unknown_root_)))
        return fail(ConfigStoreError::kPersistence, "configuration persistence failed");
    namespaces_ = std::move(next_namespaces);
    config_ = next_config;
    writeback_pending_ = false;
    return {};
}

std::vector<std::string> ConfigStore::registered_plugin_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registered_plugin_ids_;
}

}  // namespace qol::features::core_settings
