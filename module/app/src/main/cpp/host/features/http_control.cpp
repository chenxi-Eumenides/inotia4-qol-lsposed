#include "http_control.h"
#include "production.h"
#include <charconv>
#include <cstring>

namespace qol::host {
namespace {
const JsonValue* member(const JsonValue& value, const char* key) {
    for (const auto& item : value.object) if (item.first == key) return &item.second;
    return nullptr;
}
QolResult body(contracts::HttpRes& response, uint16_t status, const std::string& json) {
    response.status = status;
    response.body_size = static_cast<uint32_t>(json.size());
    if (!response.body || response.body_capacity < json.size()) return contracts::error(kQolErrBlocked, nullptr, 0);
    std::memcpy(response.body, json.data(), json.size());
    return contracts::ok();
}
QolResult rejected(contracts::HttpRes& response, QolResult error) {
    const uint16_t status = error.code == kQolErrNotReady ? 503 :
        error.code == kQolErrNotFound ? 404 : error.code == kQolErrBusy ? 429 :
        error.code == kQolErrInternal ? 500 : 400;
    return body(response, status, "{\"ok\":false,\"error_code\":" + std::to_string(error.code) + "}");
}
std::string receipt_json(const contracts::FeatureReceipt& receipt) {
    return "{\"ok\":true,\"request_id\":" + std::to_string(receipt.request_id) +
        ",\"revision\":" + std::to_string(receipt.revision) +
        ",\"admission\":\"" + (receipt.admission == contracts::FeatureAdmission::kAdmitted ? "admitted" : "rejected") +
        "\",\"persistence\":\"" + (receipt.persistence == contracts::FeaturePersistence::kPending ? "pending" :
         receipt.persistence == contracts::FeaturePersistence::kPersisted ? "persisted" : "failed") +
        "\",\"actual\":\"" + (receipt.actual == contracts::FeatureActual::kPending ? "pending" : "not_submitted") +
        "\",\"error_code\":" + std::to_string(receipt.error_code) + "}";
}
}
bool feature_control_http_route(const contracts::HttpReq& request) {
    const std::string path(request.path, request.path_len);
    return path == "/api/config/list" || path == "/api/config/set" ||
        path == "/api/op/features/lifecycle" || path.rfind("/api/config/receipt/", 0) == 0;
}
QolResult feature_control_http_dispatch(const contracts::HttpReq& request, contracts::HttpRes& response) {
    auto& controller = production_feature_controller();
    const std::string path(request.path, request.path_len);
    if (request.method == contracts::HttpMethod::kGet && path == "/api/config/list") {
        uint32_t size = 0;
        const auto result = controller.read_registered(reinterpret_cast<char*>(response.body), response.body_capacity, &size);
        if (!contracts::succeeded(result)) return rejected(response, result);
        response.status = 200;
        response.body_size = size;
        return contracts::ok();
    }
    if (request.method == contracts::HttpMethod::kGet && path.rfind("/api/config/receipt/", 0) == 0) {
        const auto text = path.substr(sizeof("/api/config/receipt/") - 1);
        uint64_t id = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            return rejected(response, contracts::error(kQolErrInvalidArgument, nullptr, 0));
        contracts::FeatureReceipt receipt{};
        const auto result = controller.receipt(id, &receipt);
        return contracts::succeeded(result) ? body(response, 200, receipt_json(receipt)) : rejected(response, result);
    }
    if (request.method != contracts::HttpMethod::kPost || (!request.body && request.body_len))
        return body(response, 405, "{\"ok\":false,\"error\":\"method not allowed\"}");
    JsonValue envelope;
    const auto parsed = features::core_settings::parse_json_document(
        std::string(request.body ? reinterpret_cast<const char*>(request.body) : "", request.body_len), &envelope);
    if (!parsed.ok() || envelope.kind != JsonValue::Kind::kObject)
        return rejected(response, contracts::error(kQolErrInvalidArgument, nullptr, 0));
    contracts::FeatureReceipt receipt{};
    QolResult result = contracts::error(kQolErrInvalidArgument, nullptr, 0);
    if (path == "/api/op/features/lifecycle") {
        const auto* id = member(envelope, "id");
        const auto* action = member(envelope, "action");
        if (envelope.object.size() != 2 || !id || !action || id->kind != JsonValue::Kind::kString ||
            action->kind != JsonValue::Kind::kString || (action->scalar != "activate" && action->scalar != "deactivate"))
            return rejected(response, result);
        result = controller.request_desired(id->scalar.c_str(), action->scalar == "activate", &receipt);
    } else if (path == "/api/config/set") {
        const auto* id = member(envelope, "plugin_id");
        const auto* field = member(envelope, "field");
        const auto* key = member(envelope, "key");
        const auto* value = member(envelope, "value");
        if (envelope.object.size() != 4 || !id || !field || !key || !value ||
            id->kind != JsonValue::Kind::kString || field->kind != JsonValue::Kind::kString ||
            key->kind != JsonValue::Kind::kString || key->scalar.empty()) return rejected(response, result);
        if (field->scalar == "config") {
            const auto json = features::core_settings::serialize_json_value(*value);
            result = controller.request_set_config(id->scalar.c_str(), key->scalar.c_str(), json.c_str(), json.size(), &receipt);
        } else if (value->kind == JsonValue::Kind::kBoolean) {
            if (field->scalar == "feature") result = controller.request_set_feature(id->scalar.c_str(), key->scalar.c_str(), value->boolean, &receipt);
            if (field->scalar == "desired" && key->scalar == "enable") result = controller.request_desired(id->scalar.c_str(), value->boolean, &receipt);
        }
    }
    return contracts::succeeded(result) ? body(response, 200, receipt_json(receipt)) : rejected(response, result);
}
}  // namespace qol::host
