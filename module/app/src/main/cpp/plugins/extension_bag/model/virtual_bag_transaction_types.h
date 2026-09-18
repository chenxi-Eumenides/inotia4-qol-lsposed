#pragma once

struct Item {
    int category = 0;
    int count = 0;
    uint16_t payload_size = 0;
    std::array<uint8_t, kSerializedItemBuffer> payload{};
};

constexpr uint8_t kTransferOriginalToExtension = 0;
constexpr uint8_t kTransferExtensionToOriginal = 1;
constexpr uint8_t kTransferExtensionToExtension = 2;

constexpr uint8_t kJournalStagePrepared = 0;
constexpr uint8_t kJournalStageOriginalSaved = 1;
constexpr uint8_t kJournalStageSidecarCommitted = 2;
constexpr const char* kJournalSectionName = "extensionbags.journal";
constexpr int kJournalSectionVersion = 1;
constexpr size_t kMaxTransactionIdChars = 64;

struct JournalRecord {
    bool valid = false;
    uint8_t stage = kJournalStagePrepared;
    uint64_t generation = 0;
    char transaction_id[kMaxTransactionIdChars + 1]{};
    uint8_t direction = kTransferOriginalToExtension;
    uint8_t src_bag = 0;
    uint8_t src_slot = 0;
    uint8_t dst_bag = 0;
    uint8_t dst_slot = 0;
    uint16_t payload_size = 0;
    std::array<uint8_t, kSerializedItemBuffer> payload{};
    uint16_t source_payload_size = 0;
    std::array<uint8_t, kSerializedItemBuffer> source_payload{};
};

struct WorldProbe {
    bool original_slot_holds_payload = false;
};

struct PendingTransfer {
    bool valid = false;
    uint8_t direction = kTransferOriginalToExtension;
    uint8_t src_bag = 0;
    uint8_t src_slot = 0;
    uint8_t dst_bag = 0;
    uint8_t dst_slot = 0;
    uint16_t payload_size = 0;
    std::array<uint8_t, kSerializedItemBuffer> payload{};
    uint16_t source_payload_size = 0;
    std::array<uint8_t, kSerializedItemBuffer> source_payload{};
    char transaction_id[kMaxTransactionIdChars + 1]{};
};

constexpr size_t kMaxIsolationRecords = 8;

namespace isolation_reason {
constexpr const char* kPayloadInvalid = "payload_invalid";
constexpr const char* kInvalidTransactionDomain = "invalid_transaction_domain";
constexpr const char* kInvalidPayload = "invalid_payload";
constexpr const char* kLoadFailed = "load_failed";
constexpr const char* kInsertFailed = "insert_failed";
constexpr const char* kSlotNotFound = "slot_not_found";
constexpr const char* kJournalInvalid = "journal_invalid";
}

struct IsolationRecord {
    bool valid = false;
    uint64_t sequence = 0;
    uint64_t observed_at_ms = 0;
    const char* reason = "";
    char transaction_id[kMaxTransactionIdChars + 1]{};
    uint64_t generation = 0;
    int direction = -1;
    int phase = -1;
    int src_bag = -1;
    int src_slot = -1;
    int dst_bag = -1;
    int dst_slot = -1;
    uint16_t payload_size = 0;
    std::array<uint8_t, kSerializedItemBuffer> payload{};
    uint16_t source_payload_size = 0;
    std::array<uint8_t, kSerializedItemBuffer> source_payload{};
    const char* detail = "";
};
