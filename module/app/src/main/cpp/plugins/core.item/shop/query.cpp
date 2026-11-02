#include "plugins/core.item/shop/query.h"

#include <cstring>
#include <string>
#include <utility>

#include "plugins/core.item/http/json_lite.h"
#include "plugins/core.item/http/response.h"
#include "plugins/core.item/runtime/core_item_internal.h"
#include "plugins/core.item/shop/snapshot.h"

namespace qol::features::core_item::shop {
namespace {

using contracts::QolItemStaticTextKind;

struct Obj {
    std::string body;
    bool first = true;

    void integer(const char* name, long long value) {
        if (!first) body += ",";
        first = false;
        body += "\"";
        body += name;
        body += "\":" + std::to_string(value);
    }

    void text(const char* name, const char* value) {
        if (!first) body += ",";
        first = false;
        body += "\"";
        body += name;
        body += "\":\"";
        body += value;
        body += "\"";
    }

    std::string object() const { return "{" + body + "}"; }
};

const char* static_item_name(const contracts::QolItemPort* port, int32_t category) {
    if (port == nullptr || port->read.static_text == nullptr) return nullptr;
    const char* text = nullptr;
    if (!contracts::succeeded(port->read.static_text(
            static_cast<int32_t>(QolItemStaticTextKind::kItemName), category, &text))) {
        return nullptr;
    }
    return text;
}

bool member_int(const JsonValue& object, const char* key, long long* out) {
    const JsonValue* value = object.find(key);
    return value != nullptr && value->as_int(out);
}

bool enrich_shop_items(const char* json, uint32_t size, const contracts::QolItemPort* port,
                       std::string* out) {
    if (out == nullptr) return false;
    JsonValue root;
    if (!json_parse(json, size, &root) || root.type != JsonValue::Type::kObject) return false;
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
            const char* name = static_item_name(port, static_cast<int32_t>(category));
            if (name != nullptr) entry.text("name", name);
        }
        result += entry.object();
    }
    result += "]}";
    *out = std::move(result);
    return true;
}

}  // namespace

QolResult query_handler(const contracts::HttpReq* request, contracts::HttpRes* response,
                        void* ctx) {
    (void)ctx;
    if (request == nullptr || response == nullptr) return response_error(response, 400, "bad request");

    ShopSnapshot snapshot{};
    if (!g_snapshot.read(snapshot)) return response_error(response, 503, "not ready");
    if (snapshot.state == SnapshotState::kUnavailable) {
        return response_error(response, 503, "not ready");
    }
    if (snapshot.state == SnapshotState::kNotInGame) {
        return response_body(response, snapshot.json, snapshot.json_size);
    }
    if (snapshot.state != SnapshotState::kReady) {
        return response_error(response, 503, "not ready");
    }

    std::string body;
    if (!enrich_shop_items(snapshot.json, snapshot.json_size, g_port, &body)) {
        return response_error(response, 500, "internal error");
    }
    return response_body(response, body.data(), static_cast<uint32_t>(body.size()));
}

}  // namespace qol::features::core_item::shop
