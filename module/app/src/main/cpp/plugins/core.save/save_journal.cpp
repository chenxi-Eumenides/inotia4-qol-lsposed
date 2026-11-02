#include "save_journal.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace qol::features::core_save {
namespace {
uint32_t crc32(const uint8_t* bytes, std::size_t size) {
    uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}
void append(std::vector<uint8_t>& bytes, uint64_t v, unsigned width) {
    for (unsigned i = width; i != 0; --i) bytes.push_back(static_cast<uint8_t>(v >> ((i - 1) * 8)));
}
struct Reader {
    const uint8_t* bytes;
    std::size_t size;
    std::size_t pos = 0;
    bool integer(unsigned width, uint64_t* out) {
        if (width > size - pos) return false;
        *out = 0;
        for (unsigned i = 0; i < width; ++i) *out = (*out << 8) | bytes[pos++];
        return true;
    }
};
bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 64 || name == "module.save" || name.compare(0, 12, "module.save.") == 0)
        return false;
    for (const unsigned char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    return true;
}
bool valid_evidence(const SaveJournal& journal) {
    const bool hash = std::any_of(std::begin(journal.evidence.sha256), std::end(journal.evidence.sha256),
                                  [](uint8_t v) { return v != 0; });
    if (journal.stage == JournalStage::kPrepared) return !hash && journal.evidence.file_length == 0;
    return journal.stage == JournalStage::kOriginalConfirmed && hash;
}
bool utf8(const uint8_t* bytes, std::size_t size) {
    for (std::size_t i = 0; i < size;) {
        const auto first = bytes[i++];
        if (first < 0x80) continue;
        unsigned count = 0;
        uint32_t code = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 31; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 15; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 7; minimum = 0x10000; }
        else return false;
        if (count > size - i) return false;
        for (unsigned j = 0; j < count; ++j) {
            const auto c = bytes[i++];
            if ((c & 0xc0) != 0x80) return false;
            code = (code << 6) | (c & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
struct Json {
    enum Kind { kObject, kArray, kString, kInteger, kOther } kind = kOther;
    std::map<std::string, Json> object;
    std::vector<Json> array;
    std::string text;
    uint64_t number = 0;
};
// v1 仅识别有界证据，不构造可提交的 payload，也不调用任何存储接口。
class JsonReader {
public:
    JsonReader(const uint8_t* bytes, std::size_t size) : bytes_(bytes), size_(size) {}
    bool parse(Json* out) { return value(out, 0) && (space(), pos_ == size_); }
private:
    const uint8_t* bytes_;
    std::size_t size_, pos_ = 0;
    void space() { while (pos_ < size_ && (bytes_[pos_] == ' ' || bytes_[pos_] == '\n' || bytes_[pos_] == '\r' || bytes_[pos_] == '\t')) ++pos_; }
    bool take(char c) { space(); if (pos_ == size_ || bytes_[pos_] != c) return false; ++pos_; return true; }
    bool hex(uint32_t* code) {
        if (4 > size_ - pos_) return false;
        *code = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const auto c = bytes_[pos_++];
            unsigned v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return false;
            *code = (*code << 4) | v;
        }
        return true;
    }
    bool string(std::string* out) {
        if (!take('"')) return false;
        while (pos_ < size_) {
            auto c = bytes_[pos_++];
            if (c == '"') return true;
            if (c < 32) return false;
            if (c != '\\') { out->push_back(static_cast<char>(c)); continue; }
            if (pos_ == size_) return false;
            c = bytes_[pos_++];
            switch (c) {
                case '"': case '\\': case '/': out->push_back(static_cast<char>(c)); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t code = 0;
                    if (!hex(&code)) return false;
                    if (code >= 0xd800 && code <= 0xdbff) {
                        if (2 > size_ - pos_ || bytes_[pos_++] != '\\' || bytes_[pos_++] != 'u') return false;
                        uint32_t low = 0;
                        if (!hex(&low) || low < 0xdc00 || low > 0xdfff) return false;
                        code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                    } else if (code >= 0xdc00 && code <= 0xdfff) return false;
                    if (code < 0x80) out->push_back(static_cast<char>(code));
                    else {
                        const unsigned continuation = code < 0x800 ? 1 : (code < 0x10000 ? 2 : 3);
                        out->push_back(static_cast<char>((continuation == 1 ? 0xc0 : continuation == 2 ? 0xe0 : 0xf0) | (code >> (continuation * 6))));
                        for (unsigned i = continuation; i > 0; --i) out->push_back(static_cast<char>(0x80 | ((code >> ((i - 1) * 6)) & 63)));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Json* out, unsigned depth) {
        if (depth > 32) return false;
        space();
        if (pos_ == size_) return false;
        if (bytes_[pos_] == '"') { out->kind = Json::kString; return string(&out->text); }
        if (bytes_[pos_] == '{') {
            ++pos_; out->kind = Json::kObject;
            if (take('}')) return true;
            do {
                std::string key;
                Json child;
                if (!string(&key) || !take(':') || !value(&child, depth + 1) ||
                    !out->object.emplace(std::move(key), std::move(child)).second) return false;
                if (take('}')) return true;
            } while (take(','));
            return false;
        }
        if (bytes_[pos_] == '[') {
            ++pos_; out->kind = Json::kArray;
            if (take(']')) return true;
            do {
                Json child;
                if (!value(&child, depth + 1)) return false;
                out->array.push_back(std::move(child));
                if (take(']')) return true;
            } while (take(','));
            return false;
        }
        if (bytes_[pos_] >= '0' && bytes_[pos_] <= '9') {
            out->kind = Json::kInteger;
            const auto start = pos_;
            do {
                const unsigned digit = bytes_[pos_++] - '0';
                if (out->number > (UINT64_MAX - digit) / 10) return false;
                out->number = out->number * 10 + digit;
            } while (pos_ < size_ && bytes_[pos_] >= '0' && bytes_[pos_] <= '9');
            return pos_ - start == 1 || bytes_[start] != '0';
        }
        for (const auto literal : {"true", "false", "null"}) {
            const auto length = std::strlen(literal);
            if (length <= size_ - pos_ && std::memcmp(bytes_ + pos_, literal, length) == 0) {
                pos_ += length; return true;
            }
        }
        return false;
    }
};
const Json* field(const Json& json, const char* key, Json::Kind kind) {
    const auto it = json.object.find(key);
    return json.kind == Json::kObject && it != json.object.end() && it->second.kind == kind ? &it->second : nullptr;
}
bool base64(const std::string& text) {
    if (text.size() % 4 != 0) return false;
    const auto digit = [](char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const auto a = digit(text[i]), b = digit(text[i+1]);
        if (a < 0 || b < 0) return false;
        if (text[i+2] == '=') {
            if (text[i+3] != '=' || i + 4 != text.size() || (b & 15) != 0) return false;
        } else {
            const auto c = digit(text[i+2]);
            if (c < 0) return false;
            if (text[i+3] == '=') { if (i + 4 != text.size() || (c & 3) != 0) return false; }
            else if (digit(text[i+3]) < 0) return false;
        }
    }
    return true;
}
bool legacy(int slot, const uint8_t* bytes, std::size_t size) {
    if (!utf8(bytes, size)) return false;
    Json json;
    if (!JsonReader(bytes, size).parse(&json)) return false;
    const auto tx = field(json, "transactionId", Json::kString);
    const auto stored_slot = field(json, "slot", Json::kInteger);
    const auto stage = field(json, "stage", Json::kInteger);
    const auto sections = field(json, "sections", Json::kArray);
    if (tx == nullptr || tx->text.empty() || stored_slot == nullptr || stored_slot->number != static_cast<uint64_t>(slot) ||
        stage == nullptr || stage->number != 0 || sections == nullptr || sections->array.empty()) return false;
    std::set<std::string> names;
    for (const auto& section : sections->array) {
        const auto name = field(section, "name", Json::kString);
        const auto version = field(section, "version", Json::kInteger);
        const auto payload = field(section, "payload", Json::kString);
        if (name == nullptr || !valid_name(name->text) || !names.insert(name->text).second ||
            version == nullptr || version->number == 0 || version->number > UINT16_MAX ||
            payload == nullptr || !base64(payload->text)) return false;
    }
    return true;
}
}

bool encode_save_journal(const SaveJournal& journal, std::vector<uint8_t>* out) {
    if (out == nullptr || journal.tx_id == 0 || journal.slot < 0 || journal.slot > 2 || !valid_evidence(journal)) return false;
    uint64_t length = 92;
    std::set<std::string> names;
    for (const auto& entry : journal.entries) {
        if (!valid_name(entry.name) || entry.version == 0 || !names.insert(entry.name).second ||
            entry.payload.size() > kSaveJournalMaxBytes) return false;
        const uint64_t extra = 16 + entry.name.size() + entry.payload.size();
        if (extra > kSaveJournalMaxBytes - length) return false;
        length += extra;
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(length));
    append(bytes, 0x53444a32, 4); append(bytes, 2, 2); append(bytes, 48, 2);
    append(bytes, length, 4); append(bytes, static_cast<uint32_t>(journal.stage), 4);
    append(bytes, journal.tx_id, 8); append(bytes, static_cast<uint32_t>(journal.slot), 4);
    append(bytes, journal.entries.size(), 4); append(bytes, journal.session_epoch, 8);
    append(bytes, journal.stage == JournalStage::kPrepared ? 0 : 1, 4); append(bytes, 40, 4);
    append(bytes, journal.evidence.file_length, 8);
    bytes.insert(bytes.end(), std::begin(journal.evidence.sha256), std::end(journal.evidence.sha256));
    for (const auto& entry : journal.entries) {
        append(bytes, entry.owner_id, 8); append(bytes, entry.name.size(), 2); append(bytes, entry.version, 2);
        append(bytes, entry.payload.size(), 4);
        bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
        bytes.insert(bytes.end(), entry.payload.begin(), entry.payload.end());
    }
    append(bytes, crc32(bytes.data(), bytes.size()), 4);
    *out = std::move(bytes);
    return true;
}
JournalDecode decode_save_journal(uint16_t version, int32_t slot, const uint8_t* bytes, uint64_t length, SaveJournal* out) {
    if (version != 1 && version != 2) return JournalDecode::kUnsupported;
    if (bytes == nullptr || length == 0 || length > kSaveJournalMaxBytes || slot < 0 || slot > 2)
        return version == 1 ? JournalDecode::kCorruptLegacy : JournalDecode::kCorrupt;
    if (version == 1) return legacy(slot, bytes, static_cast<std::size_t>(length))
        ? JournalDecode::kLegacyUnresolved : JournalDecode::kCorruptLegacy;
    if (length < 92 || out == nullptr) return JournalDecode::kCorrupt;
    Reader tail{bytes + length - 4, 4};
    uint64_t checksum = 0;
    if (!tail.integer(4, &checksum) || checksum != crc32(bytes, static_cast<std::size_t>(length - 4))) return JournalDecode::kCorrupt;
    Reader r{bytes, static_cast<std::size_t>(length - 4)};
    uint64_t magic=0, schema=0, header=0, total=0, stage=0, stored_slot=0, count=0, kind=0, evidence_size=0;
    SaveJournal candidate;
    if (!r.integer(4,&magic) || magic != 0x53444a32 || !r.integer(2,&schema) || schema != 2 ||
        !r.integer(2,&header) || header != 48 || !r.integer(4,&total) || total != length ||
        !r.integer(4,&stage) || stage > 1 || !r.integer(8,&candidate.tx_id) || candidate.tx_id == 0 ||
        !r.integer(4,&stored_slot) || stored_slot != static_cast<uint32_t>(slot) ||
        !r.integer(4,&count) || !r.integer(8,&candidate.session_epoch) ||
        !r.integer(4,&kind) || kind != stage || !r.integer(4,&evidence_size) || evidence_size != 40 ||
        !r.integer(8,&candidate.evidence.file_length) || 32 > r.size - r.pos) return JournalDecode::kCorrupt;
    candidate.slot = slot;
    candidate.stage = static_cast<JournalStage>(stage);
    std::memcpy(candidate.evidence.sha256, bytes + r.pos, 32); r.pos += 32;
    if (!valid_evidence(candidate) || count > (r.size - r.pos) / 17) return JournalDecode::kCorrupt;
    std::set<std::string> names;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t owner=0, name_size=0, section_version=0, payload_size=0;
        if (!r.integer(8,&owner) || !r.integer(2,&name_size) || name_size == 0 || name_size > 64 ||
            !r.integer(2,&section_version) || section_version == 0 || !r.integer(4,&payload_size) ||
            payload_size > kSaveJournalMaxBytes || name_size > r.size - r.pos) return JournalDecode::kCorrupt;
        std::string name(reinterpret_cast<const char*>(bytes + r.pos), static_cast<std::size_t>(name_size)); r.pos += name_size;
        if (!valid_name(name) || !names.insert(name).second || payload_size > r.size - r.pos) return JournalDecode::kCorrupt;
        JournalEntry entry{owner, std::move(name), static_cast<uint16_t>(section_version), {}};
        entry.payload.assign(bytes + r.pos, bytes + r.pos + payload_size); r.pos += payload_size;
        candidate.entries.push_back(std::move(entry));
    }
    if (r.pos != r.size) return JournalDecode::kCorrupt;
    *out = std::move(candidate);
    return JournalDecode::kValid;
}
LeaseAction SaveRecoveryState::before_original_failure(SaveFailure) {
    if (health_ != SlotHealth::kHealthy) return LeaseAction::kHold;
    return LeaseAction::kRelease;
}
bool SaveRecoveryState::original_confirmed(const SaveJournal& captured) {
    if (health_ != SlotHealth::kHealthy || captured.stage != JournalStage::kOriginalConfirmed) return false;
    std::vector<uint8_t> checked;
    if (!encode_save_journal(captured, &checked)) return false;
    captured_ = captured; // 深拷贝；重试不读取运行时，也不重新 capture。
    retries_ = 0; last_frame_ = UINT64_MAX; health_ = SlotHealth::kCommitPending;
    return true;
}
void SaveRecoveryState::begin_pending() {
    if (health_ != SlotHealth::kHealthy) return;
    retries_ = 0; last_frame_ = UINT64_MAX; health_ = SlotHealth::kCommitPending;
}
bool SaveRecoveryState::retry_frame(uint64_t frame) {
    if (health_ != SlotHealth::kCommitPending || retries_ >= 3 ||
        (retries_ != 0 && frame <= last_frame_)) return false;
    last_frame_ = frame; ++retries_; health_ = SlotHealth::kRecoveryRetrying;
    return true;
}
LeaseAction SaveRecoveryState::retry_result(bool committed) {
    if (health_ != SlotHealth::kRecoveryRetrying) return LeaseAction::kHold;
    if (committed) { health_ = SlotHealth::kHealthy; captured_ = {}; return LeaseAction::kRelease; }
    health_ = retries_ == 3 ? SlotHealth::kDegraded : SlotHealth::kCommitPending;
    return LeaseAction::kHold;
}
void SaveRecoveryState::degrade() { health_ = SlotHealth::kDegraded; }
LeaseAction SaveRecoveryState::explicit_recovery_finished(bool committed) {
    if (!committed) return LeaseAction::kHold;
    health_ = SlotHealth::kHealthy; captured_ = {}; return LeaseAction::kRelease;
}
bool may_auto_recover(const SaveJournal& journal, const contracts::QolSaveFileEvidence& current) {
    return journal.stage == JournalStage::kOriginalConfirmed && valid_evidence(journal) &&
           journal.evidence.file_length == current.file_length && std::memcmp(journal.evidence.sha256, current.sha256, 32) == 0;
}
}  // namespace qol::features::core_save
