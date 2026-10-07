#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "contracts/save_file_evidence.h"

namespace qol::features::core_save {
constexpr const char* kSaveJournalSection = "module.save.journal";
constexpr uint16_t kSaveJournalVersion = 2;
constexpr uint32_t kSaveJournalMaxBytes = 1024 * 1024;
enum class JournalStage : uint32_t { kPrepared = 0, kOriginalConfirmed = 1 };
struct JournalEntry {
    uint64_t owner_id; // 只记来源身份，不是重启后的授权 token。
    std::string name;
    uint16_t version;
    std::vector<uint8_t> payload;
};
struct SaveJournal {
    uint64_t tx_id = 0;
    int32_t slot = -1;
    uint64_t session_epoch = 0;
    JournalStage stage = JournalStage::kPrepared;
    contracts::QolSaveFileEvidence evidence{};
    std::vector<JournalEntry> entries;
};
enum class JournalDecode { kValid, kCorrupt, kUnsupported, kLegacyUnresolved, kCorruptLegacy };
bool encode_save_journal(const SaveJournal& journal, std::vector<uint8_t>* out);
JournalDecode decode_save_journal(uint16_t section_version, int32_t expected_slot,
                                 const uint8_t* bytes, uint64_t size, SaveJournal* out);
enum class SlotHealth { kHealthy, kCommitPending, kRecoveryRetrying, kDegraded };
enum class LeaseAction { kRelease, kHold };
enum class SaveFailure { kPreparedWrite, kCancelledBeforeOriginal, kOriginalFailed };
// 纯状态容器；调用方执行返回的租约动作，不在此进行 IO 或调用原版保存。
class SaveRecoveryState {
public:
    LeaseAction before_original_failure(SaveFailure);
    bool original_confirmed(const SaveJournal& captured);
    bool retry_frame(uint64_t frame);
    LeaseAction retry_result(bool committed);
    void degrade();
    LeaseAction explicit_recovery_finished(bool committed);
    SlotHealth health() const { return health_; }
    bool permits_save() const { return health_ == SlotHealth::kHealthy; }
    bool permits_transition() const { return health_ == SlotHealth::kHealthy; }
    bool permits_import() const { return health_ == SlotHealth::kHealthy; }
    bool permits_data_write() const { return health_ == SlotHealth::kHealthy; }
    const SaveJournal& captured() const { return captured_; }
    uint32_t retries() const { return retries_; }
private:
    SlotHealth health_ = SlotHealth::kHealthy;
    SaveJournal captured_;
    uint32_t retries_ = 0;
    uint64_t last_frame_ = UINT64_MAX;
};
// prepared 的跨文件崩溃窗口、未知/v1 证据均不自动提交或删除。
bool may_auto_recover(const SaveJournal&, const contracts::QolSaveFileEvidence& current);
}  // namespace qol::features::core_save
