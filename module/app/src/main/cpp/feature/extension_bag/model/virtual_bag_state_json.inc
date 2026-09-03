inline std::string state_json(const State& state, bool include_isolations = false) {
    std::string json = "{\"mode\":\"";
    json += state.mode == Mode::kModule ? "module" : "original";
    json += "\",\"originalSelected\":" + std::to_string(state.original_selected);
    json += ",\"types\":[";
    for (int index = 0; index < kBagCount; ++index) {
        if (index > 0) json += ',';
        json += std::to_string(state.types[index]);
    }
    json += "],\"capacities\":[";
    for (int index = 0; index < kBagCount; ++index) {
        if (index > 0) json += ',';
        json += std::to_string(state.capacities[index]);
    }
    json += "],\"selected\":" + std::to_string(state.selected);
    json += ",\"inspected\":" + std::to_string(state.inspected);
    json += ",\"infoBag\":" + std::to_string(state.info_bag);
    json += ",\"items\":[";
    for (int bag = 0; bag < kBagCount; ++bag) {
        if (bag > 0) json += ',';
        json += '[';
        for (int slot = 0; slot < kSlotCount; ++slot) {
            if (slot > 0) json += ',';
            const Item& item = state.items[bag][slot];
            json += "{\"category\":" + std::to_string(item.category) +
                    ",\"count\":" + std::to_string(item.count);
            if (item.payload_size > 0) {
                json += ",\"payload\":\"" +
                        base64_encode(item.payload.data(), item.payload_size) + "\"";
            }
            json += '}';
        }
        json += ']';
    }
    json += "]";
    if (state.pending.valid) {
        json += ",\"pending\":{\"direction\":" + std::to_string(state.pending.direction);
        json += ",\"srcBag\":" + std::to_string(state.pending.src_bag);
        json += ",\"srcSlot\":" + std::to_string(state.pending.src_slot);
        json += ",\"dstBag\":" + std::to_string(state.pending.dst_bag);
        json += ",\"dstSlot\":" + std::to_string(state.pending.dst_slot);
        if (state.pending.transaction_id[0] != '\0') {
            json += ",\"transactionId\":\"";
            json += state.pending.transaction_id;
            json += "\"";
        }
        json += ",\"payload\":\"" +
                base64_encode(state.pending.payload.data(), state.pending.payload_size) + "\"}";
        if (state.pending.source_payload_size > 0) {
            json.pop_back();
            json += ",\"sourcePayload\":\"" +
                    base64_encode(state.pending.source_payload.data(),
                                  state.pending.source_payload_size) + "\"}";
        }
    }
    // P4.5 只读诊断：仅开发期状态读取（API status）开启；sidecar 写入路径保持
    // false（隔离记录落盘时机由 P7 决定）。
    if (include_isolations && state.isolation_count > 0) {
        json += ",\"isolations\":[";
        for (size_t index = 0; index < state.isolation_count; ++index) {
            const size_t position =
                (state.isolation_next + kMaxIsolationRecords - state.isolation_count + index) %
                kMaxIsolationRecords;
            const IsolationRecord& record = state.isolations[position];
            if (index > 0) json += ',';
            json += "{\"recordId\":" + std::to_string(record.sequence);
            json += ",\"observedAt\":" + std::to_string(record.observed_at_ms);
            json += ",\"reason\":\"" + std::string(record.reason) + "\"";
            json += ",\"transactionId\":\"" + std::string(record.transaction_id) + "\"";
            json += ",\"generation\":" + std::to_string(record.generation);
            json += ",\"direction\":" + std::to_string(record.direction);
            json += ",\"phase\":" + std::to_string(record.phase);
            json += ",\"srcBag\":" + std::to_string(record.src_bag);
            json += ",\"srcSlot\":" + std::to_string(record.src_slot);
            json += ",\"dstBag\":" + std::to_string(record.dst_bag);
            json += ",\"dstSlot\":" + std::to_string(record.dst_slot);
            json += ",\"payloadLength\":" + std::to_string(record.payload_size);
            if (record.payload_size > 0) {
                json += ",\"payload\":\"" +
                        base64_encode(record.payload.data(), record.payload_size) + "\"";
            }
            json += ",\"sourcePayloadLength\":" + std::to_string(record.source_payload_size);
            if (record.source_payload_size > 0) {
                json += ",\"sourcePayload\":\"" +
                        base64_encode(record.source_payload.data(), record.source_payload_size) +
                        "\"";
            }
            json += ",\"detail\":\"" + std::string(record.detail) + "\"}";
        }
        json += ']';
    }
    json += "}";
    return json;
}

enum class PendingTransferParseResult {
    kOk,
    kMalformed,
    kInvalidTransactionDomain,
};

inline PendingTransferParseResult parse_pending_transfer_result(const char* json,
                                                                 PendingTransfer* out) {
    if (json == nullptr || out == nullptr) return PendingTransferParseResult::kMalformed;
    const char* key = strstr(json, "\"pending\":{");
    if (key == nullptr) return PendingTransferParseResult::kMalformed;
    PendingTransfer parsed{};
    auto parse_field = [&](const char* name, uint8_t* dst) -> bool {
        const std::string pattern = std::string("\"") + name + "\":";
        const char* pos = strstr(key, pattern.c_str());
        if (pos == nullptr) return false;
        char* end = nullptr;
        const long v = strtol(pos + pattern.size(), &end, 10);
        if (end == pos + static_cast<std::ptrdiff_t>(pattern.size()) || v < 0 || v > 0xFF) {
            return false;
        }
        *dst = static_cast<uint8_t>(v);
        return true;
    };
    if (!parse_field("direction", &parsed.direction) ||
        parsed.direction > kTransferExtensionToExtension) {
        return PendingTransferParseResult::kMalformed;
    }
    if (!parse_field("srcBag", &parsed.src_bag) ||
        !parse_field("srcSlot", &parsed.src_slot) ||
        !parse_field("dstBag", &parsed.dst_bag) ||
        !parse_field("dstSlot", &parsed.dst_slot)) {
        return PendingTransferParseResult::kMalformed;
    }
    const char* id_key = strstr(key, "\"transactionId\":\"");
    if (id_key != nullptr) {
        const char* id_begin = id_key + strlen("\"transactionId\":\"");
        const char* id_end = strchr(id_begin, '"');
        if (id_end == nullptr) return PendingTransferParseResult::kMalformed;
        const size_t id_len = static_cast<size_t>(id_end - id_begin);
        if (id_len == 0 || id_len > kMaxTransactionIdChars) {
            return PendingTransferParseResult::kMalformed;
        }
        std::memcpy(parsed.transaction_id, id_begin, id_len);
        parsed.transaction_id[id_len] = '\0';
    }
    const char* payload_key = strstr(key, "\"payload\":\"");
    if (payload_key == nullptr) return PendingTransferParseResult::kMalformed;
    const char* b64 = payload_key + strlen("\"payload\":\"");
    const char* end_quote = strchr(b64, '"');
    if (end_quote == nullptr) return PendingTransferParseResult::kMalformed;
    const size_t b64_len = static_cast<size_t>(end_quote - b64);
    if (b64_len == 0 || b64_len > 512) return PendingTransferParseResult::kMalformed;
    const int decoded = base64_decode(b64, b64_len, parsed.payload.data(), parsed.payload.size());
    if (decoded < static_cast<int>(kPayloadHeaderSize) ||
        decoded > static_cast<int>(kMaxSerializedItem)) {
        return PendingTransferParseResult::kMalformed;
    }
    parsed.payload_size = static_cast<uint16_t>(decoded);
    if (!valid_pending_transfer_payload(parsed)) {
        return PendingTransferParseResult::kMalformed;
    }
    const char* source_key = strstr(key, "\"sourcePayload\":\"");
    if (source_key != nullptr) {
        const char* source_b64 = source_key + strlen("\"sourcePayload\":\"");
        const char* source_end = strchr(source_b64, '\"');
        if (source_end == nullptr) return PendingTransferParseResult::kMalformed;
        const size_t source_len = static_cast<size_t>(source_end - source_b64);
        if (source_len == 0 || source_len > 512) return PendingTransferParseResult::kMalformed;
        const int source_decoded =
            base64_decode(source_b64, source_len, parsed.source_payload.data(),
                          parsed.source_payload.size());
        if (source_decoded < static_cast<int>(kPayloadHeaderSize) ||
            source_decoded > static_cast<int>(kMaxSerializedItem)) {
            return PendingTransferParseResult::kMalformed;
        }
        parsed.source_payload_size = static_cast<uint16_t>(source_decoded);
        if (!valid_pending_transfer_payload(parsed)) {
            return PendingTransferParseResult::kMalformed;
        }
    }
    // P4.5/D1：域检查后置到载荷解析之后，使 kInvalidTransactionDomain 的
    // out 携带 transaction_id 与 payload 原始字节，供 load 侧隔离记录保留。
    if (!valid_pending_transfer_domain(parsed)) {
        *out = parsed;
        return PendingTransferParseResult::kInvalidTransactionDomain;
    }
    parsed.valid = true;
    *out = parsed;
    return PendingTransferParseResult::kOk;
}

inline bool parse_pending_transfer(const char* json, PendingTransfer* out) {
    return parse_pending_transfer_result(json, out) == PendingTransferParseResult::kOk;
}

inline bool parse_state_json(const char* json, State* state) {
    if (json == nullptr || state == nullptr) return false;
    const char* types = strstr(json, "\"types\":[");
    if (types == nullptr) return false;
    const char* cursor = types + strlen("\"types\":[");
    State parsed{};
    for (int index = 0; index < kBagCount; ++index) {
        char* end = nullptr;
        long type = strtol(cursor, &end, 10);
        if (end == cursor || type < 0 || type > static_cast<long>(BagType::kLargeBackpack)) {
            return false;
        }
        parsed.types[index] = static_cast<uint8_t>(type);
        cursor = end;
        if (index + 1 < kBagCount) {
            if (*cursor != ',') return false;
            ++cursor;
        }
    }
    if (*cursor != ']') return false;
    const char* mode = strstr(json, "\"mode\":\"module\"");
    parsed.mode = mode == nullptr ? Mode::kOriginal : Mode::kModule;
    const char* original_selected = strstr(json, "\"originalSelected\":");
    const char* selected = strstr(json, "\"selected\":");
    const char* inspected = strstr(json, "\"inspected\":");
    if (original_selected == nullptr || selected == nullptr || inspected == nullptr) return false;
    cursor = original_selected + strlen("\"originalSelected\":");
    char* end = nullptr;
    long original = strtol(cursor, &end, 10);
    if (end == cursor || original < 0 || original >= 6) return false;
    parsed.original_selected = static_cast<int>(original);
    cursor = selected + strlen("\"selected\":");
    {
        char* selected_end = nullptr;
        long value = strtol(cursor, &selected_end, 10);
        if (selected_end == cursor || value < -1 || value > kMaxCapacity) return false;
        parsed.selected = static_cast<int>(value);
    }
    cursor = inspected + strlen("\"inspected\":");
    {
        char* inspected_end = nullptr;
        long value = strtol(cursor, &inspected_end, 10);
        if (inspected_end == cursor || value < -1 || value > kMaxCapacity) return false;
        parsed.inspected = static_cast<int>(value);
    }
    const char* info_bag = strstr(json, "\"infoBag\":");
    if (info_bag != nullptr) {
        cursor = info_bag + strlen("\"infoBag\":");
        char* info_end = nullptr;
        long value = strtol(cursor, &info_end, 10);
        if (info_end == cursor || value < -1 || value >= kBagCount) return false;
        parsed.info_bag = static_cast<int>(value);
    }
    const char* items = strstr(json, "\"items\":[");
    if (items == nullptr) return false;
    cursor = items + strlen("\"items\":[");
    for (int bag = 0; bag < kBagCount; ++bag) {
        for (int slot = 0; slot < kSlotCount; ++slot) {
            const char* object_start = strchr(cursor, '{');
            if (object_start == nullptr) return false;
            const char* object_end = strchr(object_start, '}');
            if (object_end == nullptr) return false;
            const char* category = strstr(object_start, "\"category\":");
            const char* count = strstr(object_start, "\"count\":");
            if (category == nullptr || count == nullptr || category >= object_end || count >= object_end) {
                return false;
            }
            char* category_end = nullptr;
            long category_value = strtol(category + strlen("\"category\":"), &category_end, 10);
            char* count_end = nullptr;
            long count_value = strtol(count + strlen("\"count\":"), &count_end, 10);
            if (category_end == category + static_cast<std::ptrdiff_t>(strlen("\"category\":")) ||
                count_end == count + static_cast<std::ptrdiff_t>(strlen("\"count\":")) ||
                category_value < 0 || count_value < 0) {
                return false;
            }
            Item& item = parsed.items[bag][slot];
            item.category = static_cast<int>(category_value);
            item.count = static_cast<int>(count_value);
            const char* payload_key = strstr(object_start, "\"payload\":\"");
            if (payload_key != nullptr && payload_key < object_end) {
                const char* b64 = payload_key + strlen("\"payload\":\"");
                const char* end_quote = strchr(b64, '"');
                if (end_quote == nullptr) return false;
                const size_t b64_len = static_cast<size_t>(end_quote - b64);
                if (b64_len == 0 || b64_len > 512) return false;
                const int decoded =
                    base64_decode(b64, b64_len, item.payload.data(), item.payload.size());
                if (decoded < static_cast<int>(kPayloadHeaderSize) ||
                    decoded > static_cast<int>(kMaxSerializedItem)) {
                    return false;
                }
                item.payload_size = static_cast<uint16_t>(decoded);
            }
            cursor = object_end + 1;
        }
    }
    if (strstr(json, "\"pending\":") != nullptr) {
        const PendingTransferParseResult pending_result =
            parse_pending_transfer_result(json, &parsed.pending);
        if (pending_result != PendingTransferParseResult::kOk) {
            parsed.pending = {};
        }
    }
    normalize(&parsed);
    *state = parsed;
    return true;
}

// 同 parse_state_json，但为隔离记录注入观察时间（P4.5）。
inline bool parse_state_json_at(const char* json, State* state, uint64_t observed_at_ms) {
    if (state == nullptr) return false;
    State parsed;
    if (!parse_state_json(json, &parsed)) return false;
    for (size_t index = 0; index < kMaxIsolationRecords; ++index) {
        if (parsed.isolations[index].valid && parsed.isolations[index].observed_at_ms == 0) {
            parsed.isolations[index].observed_at_ms = observed_at_ms;
        }
    }
    *state = parsed;
    return true;
}

