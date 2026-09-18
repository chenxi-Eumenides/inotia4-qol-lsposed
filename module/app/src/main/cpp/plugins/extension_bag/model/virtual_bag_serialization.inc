inline std::string base64_encode(const uint8_t* data, size_t size) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += kTable[(v >> 6) & 0x3F];
        out += kTable[v & 0x3F];
    }
    if (i + 1 == size) {
        const uint32_t v = uint32_t(data[i]) << 16;
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == size) {
        const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += kTable[(v >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

// 从 JSON 字符串字面量 [begin,end) 反转义拷贝到 out（不写 NUL，最多 cap 字节）。
// 本模块 Kotlin 侧经 org.json JSONObject.toString() 产出 JSON，会把 '/' 转义为 `\/`；
// base64 字母表不含反斜杠，故按 `\X`→`X` 处理即可（`\/`→`/`、`\\`→`\`）。返回长度，溢出返回 0。
inline size_t json_unescape_into(const char* begin, const char* end, char* out, size_t cap) {
    size_t len = 0;
    for (const char* p = begin; p < end; ++p) {
        if (*p == '\\' && p + 1 < end) ++p;
        if (len >= cap) return 0;
        out[len++] = *p;
    }
    return len;
}

// 严格 RFC4648 解码：长度必须为 4 的倍数，'=' 仅允许结尾 0-2 个，非法字符拒绝。
inline int base64_decode(const char* text, size_t len, uint8_t* out, size_t cap) {
    if (text == nullptr || len == 0 || len % 4 != 0) return 0;
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    size_t written = 0;
    for (size_t i = 0; i < len; i += 4) {
        int v[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            if (text[i + k] == '=') {
                v[k] = -2;
            } else {
                v[k] = value(text[i + k]);
                if (v[k] < 0) return 0;
            }
        }
        if (v[0] < 0 || v[1] < 0) return 0;
        const uint32_t a = uint32_t(v[0]) << 18 | uint32_t(v[1]) << 12;
        if (v[2] == -2) {
            if (v[3] != -2 || written >= cap) return 0;
            out[written++] = static_cast<uint8_t>(a >> 16);
            continue;
        }
        if (v[3] == -2) {
            if (written + 1 >= cap) return 0;
            const uint32_t b = a | uint32_t(v[2]) << 6;
            out[written++] = static_cast<uint8_t>(b >> 16);
            out[written++] = static_cast<uint8_t>((b >> 8) & 0xFF);
            continue;
        }
        if (written + 2 >= cap) return 0;
        const uint32_t b = a | uint32_t(v[2]) << 6 | uint32_t(v[3]);
        out[written++] = static_cast<uint8_t>(b >> 16);
        out[written++] = static_cast<uint8_t>((b >> 8) & 0xFF);
        out[written++] = static_cast<uint8_t>(b & 0xFF);
    }
    return static_cast<int>(written);
}

inline JournalRecord journal_from_pending(const PendingTransfer& pending, uint64_t generation) {
    JournalRecord journal{};
    journal.valid = pending.valid;
    journal.stage = kJournalStagePrepared;
    journal.generation = generation;
    std::strncpy(journal.transaction_id, pending.transaction_id, kMaxTransactionIdChars);
    journal.transaction_id[kMaxTransactionIdChars] = '\0';
    journal.direction = pending.direction;
    journal.src_bag = pending.src_bag;
    journal.src_slot = pending.src_slot;
    journal.dst_bag = pending.dst_bag;
    journal.dst_slot = pending.dst_slot;
    journal.payload_size = pending.payload_size;
    journal.payload = pending.payload;
    journal.source_payload_size = pending.source_payload_size;
    journal.source_payload = pending.source_payload;
    return journal;
}

inline std::string journal_json(const JournalRecord& journal) {
    std::string json = "{\"transactionId\":\"";
    json += journal.transaction_id;
    json += "\",\"stage\":" + std::to_string(journal.stage);
    json += ",\"generation\":" + std::to_string(journal.generation);
    json += ",\"direction\":" + std::to_string(journal.direction);
    json += ",\"srcBag\":" + std::to_string(journal.src_bag);
    json += ",\"srcSlot\":" + std::to_string(journal.src_slot);
    json += ",\"dstBag\":" + std::to_string(journal.dst_bag);
    json += ",\"dstSlot\":" + std::to_string(journal.dst_slot);
    if (journal.payload_size > 0) {
        json += ",\"payload\":\"" +
                base64_encode(journal.payload.data(), journal.payload_size) + "\"";
    }
    if (journal.source_payload_size > 0) {
        json += ",\"sourcePayload\":\"" +
                base64_encode(journal.source_payload.data(), journal.source_payload_size) + "\"";
    }
    json += "}";
    return json;
}

inline bool parse_journal_json(const char* json, JournalRecord* out) {
    if (json == nullptr || out == nullptr) return false;
    JournalRecord parsed{};
    const char* id_key = strstr(json, "\"transactionId\":\"");
    if (id_key == nullptr) return false;
    const char* id_begin = id_key + strlen("\"transactionId\":\"");
    const char* id_end = strchr(id_begin, '"');
    if (id_end == nullptr) return false;
    const size_t id_len = static_cast<size_t>(id_end - id_begin);
    if (id_len == 0 || id_len > kMaxTransactionIdChars) return false;
    std::memcpy(parsed.transaction_id, id_begin, id_len);
    parsed.transaction_id[id_len] = '\0';
    auto parse_u64 = [&](const char* name, uint64_t* dst) -> bool {
        const std::string pattern = std::string("\"") + name + "\":";
        const char* pos = strstr(json, pattern.c_str());
        if (pos == nullptr) return false;
        char* end = nullptr;
        const unsigned long long v = strtoull(pos + pattern.size(), &end, 10);
        if (end == pos + static_cast<std::ptrdiff_t>(pattern.size())) return false;
        *dst = static_cast<uint64_t>(v);
        return true;
    };
    uint64_t stage = 0;
    if (!parse_u64("stage", &stage) || stage > kJournalStageSidecarCommitted) return false;
    parsed.stage = static_cast<uint8_t>(stage);
    if (!parse_u64("generation", &parsed.generation)) return false;
    auto parse_u8 = [&](const char* name, uint8_t* dst) -> bool {
        uint64_t v = 0;
        if (!parse_u64(name, &v) || v > 0xFF) return false;
        *dst = static_cast<uint8_t>(v);
        return true;
    };
    if (!parse_u8("direction", &parsed.direction) || parsed.direction > kTransferExtensionToOriginal) {
        return false;
    }
    if (!parse_u8("srcBag", &parsed.src_bag) || !parse_u8("srcSlot", &parsed.src_slot) ||
        !parse_u8("dstBag", &parsed.dst_bag) || !parse_u8("dstSlot", &parsed.dst_slot)) {
        return false;
    }
    const char* payload_key = strstr(json, "\"payload\":\"");
    if (payload_key != nullptr) {
        const char* b64 = payload_key + strlen("\"payload\":\"");
        const char* end_quote = strchr(b64, '"');
        if (end_quote == nullptr) return false;
        char b64_plain[512];
        const size_t b64_len = json_unescape_into(b64, end_quote, b64_plain, sizeof(b64_plain));
        if (b64_len == 0) return false;
        const int decoded =
            base64_decode(b64_plain, b64_len, parsed.payload.data(), parsed.payload.size());
    if (decoded < static_cast<int>(kPayloadHeaderSize) ||
        decoded > static_cast<int>(kMaxSerializedItem)) {
        return false;
    }
    parsed.payload_size = static_cast<uint16_t>(decoded);
    if (validate_serialized_payload_buffer(parsed.payload.data(), parsed.payload.size(),
                                           parsed.payload_size) != PayloadValidation::kOk) {
        return false;
    }
    }
    const char* source_key = strstr(json, "\"sourcePayload\":\"");
    if (source_key != nullptr) {
        const char* source_b64 = source_key + strlen("\"sourcePayload\":\"");
        const char* source_end = strchr(source_b64, '\"');
        if (source_end == nullptr) return false;
        char source_plain[512];
        const size_t source_len =
            json_unescape_into(source_b64, source_end, source_plain, sizeof(source_plain));
        if (source_len == 0) return false;
        const int source_decoded =
            base64_decode(source_plain, source_len, parsed.source_payload.data(),
                          parsed.source_payload.size());
        if (source_decoded < static_cast<int>(kPayloadHeaderSize) ||
            source_decoded > static_cast<int>(kMaxSerializedItem)) {
            return false;
        }
        parsed.source_payload_size = static_cast<uint16_t>(source_decoded);
        if (validate_serialized_payload_buffer(parsed.source_payload.data(),
                                               parsed.source_payload.size(),
                                               parsed.source_payload_size) != PayloadValidation::kOk) {
            return false;
        }
    }
    parsed.valid = true;
    if (!valid_journal_record(parsed)) return false;
    *out = parsed;
    return true;
}

