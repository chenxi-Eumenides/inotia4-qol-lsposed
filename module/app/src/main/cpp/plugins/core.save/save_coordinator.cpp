#include "save_coordinator.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "contracts/slot_data.h"
#include "contracts/slot_operation.h"
#include "save_journal.h"
#include "slot_data_provider.h"

namespace qol::features::core_save {
namespace {
using namespace qol::contracts;
constexpr auto value = [](auto v) { return static_cast<uint32_t>(v); };
constexpr auto underlying = [](auto e) { return static_cast<uint32_t>(e); };
QolResult fail(int32_t code, const char* text) {
    return error(code, text, static_cast<uint32_t>(std::strlen(text)));
}

struct CapturedSection {
    std::string name;
    uint16_t version = 0;
    std::vector<uint8_t> payload;
    uint64_t owner = 0;
};

enum class TxPhase : uint32_t {
    kIdle = 0,
    kAwaitingEvidence = 1,  // 原版已成功，evidence 未取得。
    kAwaitingReplace = 2,   // confirmed journal 已写，正式替换未提交。
    kAwaitingConfirmed = 3,
};

struct SlotTx {
    SaveRecoveryState recovery;
    uint64_t tx_id = 0;
    TxPhase phase = TxPhase::kIdle;
    QolSlotOperationLease lease{};
    QolOwnerStoreReservation reservation{};
    bool lease_held = false;
    bool reservation_held = false;
    std::vector<CapturedSection> captured;  // 捕获字节；重试不重新 capture。
    SaveJournal journal;                    // confirmed（stage1 + evidence）。
    std::vector<uint8_t> confirmed_bytes;
    QolSaveOriginalResult original{};       // 本次事务的原版真实返回（SAVE_Save 入口透传）。
    // true：由 hook 入口发起，pending 由生产帧任务推进；HTTP 票据不重复推进该 tx。
    bool frame_retry = false;
};

struct Runtime {
    std::mutex mutex;
    QolOwnerToken owner = 0;
    QolOwnerVerifyTokenFn verify = nullptr;
    const QolOwnerStoreApi* backend = nullptr;
    const QolSlotOperationApi* operation = nullptr;
    const QolSaveTransactionPort* adapter = nullptr;
    int32_t (*is_main_thread)() = nullptr;
    uint64_t next_tx = 1;
    std::vector<SlotTx> slots{3};
};
Runtime g;

bool main_thread() {
    int32_t (*is_main)() = nullptr;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        is_main = g.is_main_thread;
    }
    return is_main != nullptr && is_main() != 0;
}

SlotTx& slot_tx(int32_t slot) { return g.slots[static_cast<std::size_t>(slot)]; }

// 释放顺序冻结：先 reservation 后 operation。
void release_credentials(SlotTx& tx) {
    if (tx.reservation_held && g.backend != nullptr)
        (void)g.backend->release(g.owner, tx.reservation);
    tx.reservation_held = false;
    if (tx.lease_held && g.operation != nullptr)
        (void)g.operation->release(g.operation->host_ctx, g.owner, &tx.lease);
    tx.lease_held = false;
}

// 终态收敛 provider 的保存事务（通知 complete、释放参与者引用、按 outcome 清 dirty）。
// 每个终态恰一次；成功提交路径用 kCommitted，其余终态用对应值，degraded 用 kDegraded。
void finish_capture(SlotTx& tx, uint32_t outcome) {
    if (tx.tx_id == 0) return;
    slot_data_save_finish(tx.tx_id, outcome);
    tx.tx_id = 0;
}

QolStoreSection journal_section(const std::vector<uint8_t>& bytes) {
    return QolStoreSection{kSaveJournalSection, kSaveJournalVersion, bytes.data(),
                           static_cast<uint32_t>(bytes.size())};
}

// 条件清理：仅当盘上 journal 是本事务的 prepared（确认未写盘且身份匹配）才删除；
// 不匹配/不可读一律保留证据。
bool cleanup_prepared_journal_if_ours(SlotTx& tx, int32_t slot) {
    std::vector<uint8_t> bytes(1024 * 1024);
    uint64_t size = 0;
    uint16_t version = 0;
    const auto read = g.backend->read_section(g.owner, slot, kSaveJournalSection, bytes.data(),
                                              bytes.size(), &size, &version);
    if (!succeeded(read)) return read.code == kQolErrNotFound;
    SaveJournal on_disk;
    const auto decoded = decode_save_journal(version, slot, bytes.data(), size, &on_disk);
    if (decoded != JournalDecode::kValid || on_disk.stage != JournalStage::kPrepared ||
        on_disk.tx_id != tx.tx_id)
        return false;
    auto credential = tx.reservation;
    const auto removed = g.backend->remove_reserved(g.owner, slot, &credential, kSaveJournalSection);
    tx.reservation = credential;
    return succeeded(removed);
}

// 从 journal entries 重建捕获集（重启恢复不重新 capture）。
std::vector<CapturedSection> captured_from_journal(const SaveJournal& journal) {
    std::vector<CapturedSection> out;
    out.reserve(journal.entries.size());
    for (const auto& entry : journal.entries)
        out.push_back(CapturedSection{entry.name, entry.version, entry.payload, entry.owner_id});
    return out;
}

struct ExecuteResult {
    SaveSettle settle;
    std::string error;
};

// 线性事务主体：任何失败路径均不调用后续步骤；终态 complete 恰一次。
// frame_retry=true 表示本事务由 SAVE_Save hook 入口发起，其 pending 阶段由生产帧任务推进。
ExecuteResult save_execute_impl(int32_t slot, uint64_t frame, bool frame_retry) {
    SlotTx& tx = slot_tx(slot);
    if (!tx.recovery.permits_save()) return {SaveSettle::kFailed, "slot health blocks save"};
    // 新事务从头开始：清掉上一事务残留的捕获/日志态（凭据仅由释放路径改动）。
    tx.captured.clear();
    tx.journal = SaveJournal{};
    tx.confirmed_bytes.clear();
    tx.phase = TxPhase::kIdle;
    tx.original = {};
    tx.frame_retry = frame_retry;
    const uint64_t transaction = g.next_tx < UINT64_MAX ? g.next_tx++ : 0;
    if (transaction == 0) return {SaveSettle::kFailed, "transaction id exhausted"};
    SlotDataSaveView view{};
    const auto began = slot_data_save_begin(transaction, &view);
    if (!succeeded(began)) return {SaveSettle::kFailed, "session not capturable"};
    tx.tx_id = transaction;  // 捕获事务已开启：所有失败路径经 finish_capture 收敛。
    // 每 consumer 恰一次 capture，复制字节（capture 回调期间持参与者引用）。
    for (;;) {
        const char* section = nullptr;
        uint16_t version = 0;
        std::vector<uint8_t> payload(kQolSlotDataMaxPayloadBytes);
        uint64_t size = 0;
        QolOwnerToken owner = 0;
        const auto captured =
            slot_data_save_capture_next(transaction, &section, &version, payload.data(),
                                         payload.size(), &size, &owner);
        if (captured.code == kQolErrNotFound) break;
        if (!succeeded(captured)) {
            finish_capture(tx, value(QolSlotDataOutcome::kCancelled));
            return {SaveSettle::kFailed, "capture failed"};
        }
        payload.resize(static_cast<std::size_t>(size));
        tx.captured.push_back(CapturedSection{section, version, std::move(payload), owner});
    }
    // 三候选容量预检（真实 encoder）：prepared / confirmed / final。
    SaveJournal prepared{transaction, slot, view.session.epoch, JournalStage::kPrepared, {}, {}};
    for (const auto& item : tx.captured)
        prepared.entries.push_back({item.owner, item.name, item.version, item.payload});
    tx.journal = prepared;
    std::vector<uint8_t> prepared_bytes;
    if (!encode_save_journal(prepared, &prepared_bytes)) {
        finish_capture(tx, value(QolSlotDataOutcome::kCancelled));
        return {SaveSettle::kFailed, "prepared journal encode failed"};
    }
    const QolStoreSection prepared_section = journal_section(prepared_bytes);
    QolOwnerStoreCandidate candidates[3]{};
    candidates[0].size = sizeof(QolOwnerStoreCandidate);
    candidates[0].journal = &prepared_section;
    std::vector<QolStoreSection> final_sections;
    final_sections.reserve(tx.captured.size());
    for (const auto& item : tx.captured)
        final_sections.push_back(QolStoreSection{item.name.c_str(), item.version,
                                                 item.payload.data(),
                                                 static_cast<uint32_t>(item.payload.size())});
    const char* journal_removal = kSaveJournalSection;
    candidates[2].size = sizeof(QolOwnerStoreCandidate);
    candidates[2].replacement_count = static_cast<uint32_t>(final_sections.size());
    candidates[2].replacements = final_sections.data();
    candidates[2].removal_count = 1;
    candidates[2].removals = &journal_removal;
    // evidence 的字节区固定预留；非零占位摘要只用于 encoder 精确容量预检。
    SaveJournal confirmed_candidate = prepared;
    confirmed_candidate.stage = JournalStage::kOriginalConfirmed;
    confirmed_candidate.evidence.sha256[0] = 1;
    std::vector<uint8_t> candidate_bytes;
    if (!encode_save_journal(confirmed_candidate, &candidate_bytes)) {
        finish_capture(tx, value(QolSlotDataOutcome::kCancelled));
        return {SaveSettle::kFailed, "confirmed candidate encode failed"};
    }
    const auto confirmed_section = journal_section(candidate_bytes);
    candidates[1].size = sizeof(QolOwnerStoreCandidate);
    candidates[1].journal = &confirmed_section;
    // 先 acquire operation lease 再 reserve_in_operation。
    tx.lease.size = sizeof(tx.lease);
    const auto acquired = g.operation->acquire(g.operation->host_ctx, g.owner, 1u << slot, &tx.lease);
    if (!succeeded(acquired)) {
        finish_capture(tx, value(QolSlotDataOutcome::kCancelled));
        return {SaveSettle::kFailed, "operation unavailable"};
    }
    tx.lease_held = true;
    QolOwnerStoreReservation reservation{};
    reservation.size = sizeof(reservation);
    const auto reserved = g.backend->reserve_in_operation(g.owner, &tx.lease, slot, candidates, 3,
                                                          &reservation);
    if (!succeeded(reserved)) {
        finish_capture(tx, value(QolSlotDataOutcome::kCancelled));
        release_credentials(tx);
        return {SaveSettle::kFailed, "reservation unavailable"};
    }
    tx.reservation = reservation;
    tx.reservation_held = true;
    // prepared journal（stage 0）→ referenced 双凭据（用最新 revision 凭据）。
    auto credential = tx.reservation;
    const auto journal_written =
        g.backend->write_reserved(g.owner, slot, &credential, kSaveJournalSection,
                                  kSaveJournalVersion, prepared_bytes.data(), prepared_bytes.size());
    tx.reservation = credential;
    if (!succeeded(journal_written)) {
        finish_capture(tx, value(QolSlotDataOutcome::kCancelled));
        release_credentials(tx);
        return {SaveSettle::kFailed, "prepared journal write failed"};
    }
    (void)g.operation->set_referenced(g.operation->host_ctx, g.owner, &tx.lease, 1);
    credential = tx.reservation;
    (void)g.backend->set_referenced(g.owner, slot, &credential, 1);
    tx.reservation = credential;
    // 经 Adapter 调原版保存；QolResult ok≠原版成功，真实 int 原样保留。
    QolSaveOriginalResult original{};
    original.size = sizeof(original);
    const auto invoked = g.adapter->call_original_save(g.adapter->adapter_ctx, &tx.lease, slot,
                                                       &original);
    tx.original = original;  // SAVE_Save 入口透传原版真实 int（invoked=0 时调用方返回 0）。
    if (!succeeded(invoked) || original.invoked == 0) {
        // Adapter 未执行原版：按原版前失败处理（条件清理 + cancelled）。
        if (!cleanup_prepared_journal_if_ours(tx, slot)) {
            tx.recovery.degrade();
            finish_capture(tx, value(QolSlotDataOutcome::kDegraded));
            return {SaveSettle::kFailed, "original save not invoked; journal preserved"};
        }
        finish_capture(tx, value(QolSlotDataOutcome::kCancelled));
        release_credentials(tx);
        tx.tx_id = 0;
        return {SaveSettle::kFailed, "original save not invoked"};
    }
    if (original.result_valid != 0 && original.original_result == 0) {
        // 原版有效返回 0：不替换正式 sections；条件清理 journal，否则保留证据转 degraded。
        if (!cleanup_prepared_journal_if_ours(tx, slot)) {
            tx.recovery.degrade();
            finish_capture(tx, value(QolSlotDataOutcome::kDegraded));
            return {SaveSettle::kFailed, "original save failed; journal preserved"};
        }
        finish_capture(tx, value(QolSlotDataOutcome::kOriginalSaveFailed));
        release_credentials(tx);
        tx.tx_id = 0;
        return {SaveSettle::kFailed, "original save failed"};
    }
    // 原版成功：evidence → confirmed journal → 一次 replace_reserved。
    QolSaveFileEvidence evidence{};
    const auto evidence_read = g.adapter->read_evidence(g.adapter->adapter_ctx, &tx.lease, slot,
                                                        &evidence);
    if (!succeeded(evidence_read)) {
        tx.phase = TxPhase::kAwaitingEvidence;
        tx.recovery.begin_pending();  // 原版已成功：占住 pending 窗口，重试帧重新读取 evidence。
        (void)frame;
        return {SaveSettle::kPending, "evidence unavailable"};
    }
    tx.journal.stage = JournalStage::kOriginalConfirmed;
    tx.journal.evidence = evidence;
    if (!encode_save_journal(tx.journal, &tx.confirmed_bytes)) {
        tx.recovery.degrade();
        finish_capture(tx, value(QolSlotDataOutcome::kDegraded));
        return {SaveSettle::kFailed, "confirmed journal encode failed"};
    }
    auto write_credential = tx.reservation;
    const auto confirmed_written =
        g.backend->write_reserved(g.owner, slot, &write_credential, kSaveJournalSection,
                                  kSaveJournalVersion, tx.confirmed_bytes.data(),
                                  tx.confirmed_bytes.size());
    tx.reservation = write_credential;
    if (!succeeded(confirmed_written)) {
        tx.phase = TxPhase::kAwaitingConfirmed;
        (void)tx.recovery.original_confirmed(tx.journal);
        return {SaveSettle::kPending, "confirmed journal write deferred"};
    }
    auto replace_credential = tx.reservation;
    const auto replaced = g.backend->replace_reserved(
        g.owner, slot, &replace_credential, final_sections.data(),
        static_cast<uint32_t>(final_sections.size()), &journal_removal, 1);
    tx.reservation = replace_credential;
    if (!succeeded(replaced)) {
        tx.phase = TxPhase::kAwaitingReplace;
        (void)tx.recovery.original_confirmed(tx.journal);
        return {SaveSettle::kPending, "final replace deferred"};
    }
    // 确认提交完成：清 pending 状态 → complete(committed)（按捕获快照清 dirty）→
    // 释放顺序 reservation → operation。
    (void)tx.recovery.retry_result(true);
    finish_capture(tx, value(QolSlotDataOutcome::kCommitted));
    release_credentials(tx);
    tx.captured.clear();
    tx.confirmed_bytes.clear();
    tx.journal = SaveJournal{};
    tx.tx_id = 0;
    tx.phase = TxPhase::kIdle;
    tx.frame_retry = false;
    return {SaveSettle::kCommitted, {}};
}

}  // namespace

QolResult save_coordinator_initialize(QolOwnerToken owner, QolOwnerVerifyTokenFn verify,
                                      const QolOwnerStoreApi* backend,
                                      const QolSlotOperationApi* operation,
                                      const QolSaveTransactionPort* adapter,
                                      int32_t (*is_main)()) {
    if (owner == 0 || verify == nullptr || backend == nullptr || operation == nullptr ||
        adapter == nullptr || is_main == nullptr ||
        backend->preflight == nullptr || backend->reserve_in_operation == nullptr ||
        backend->write_reserved == nullptr || backend->replace_reserved == nullptr ||
        backend->remove_reserved == nullptr || backend->set_referenced == nullptr ||
        backend->release == nullptr || operation->acquire == nullptr ||
        operation->release == nullptr || operation->set_referenced == nullptr ||
        adapter->call_original_save == nullptr || adapter->read_evidence == nullptr)
        return fail(kQolErrInvalidArgument, "invalid coordinator deps");
    const auto identity = verify(owner);
    if (!succeeded(identity)) return identity;
    std::lock_guard<std::mutex> lock(g.mutex);
    g.owner = owner;
    g.verify = verify;
    g.backend = backend;
    g.operation = operation;
    g.adapter = adapter;
    g.is_main_thread = is_main;
    return ok();
}

void save_coordinator_shutdown() {
    std::lock_guard<std::mutex> lock(g.mutex);
    for (auto& tx : g.slots) {
        finish_capture(tx, value(QolSlotDataOutcome::kDegraded));  // 未决捕获收敛，不悬挂引用。
        release_credentials(tx);
        tx = SlotTx{};
    }
    g.next_tx = 1;
}

void save_coordinator_reset_for_test() {
    std::lock_guard<std::mutex> lock(g.mutex);
    for (auto& tx : g.slots) {
        finish_capture(tx, value(QolSlotDataOutcome::kDegraded));
        release_credentials(tx);
        tx = SlotTx{};
    }
    g.next_tx = 1;
}

bool save_coordinator_installed() {
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.backend != nullptr;
}

bool save_coordinator_permits_transition(int32_t slot) {
    std::lock_guard<std::mutex> lock(g.mutex);
    return slot >= 0 && slot <= 2 && slot_tx(slot).recovery.permits_transition();
}

bool save_coordinator_permits_save(int32_t slot) {
    std::lock_guard<std::mutex> lock(g.mutex);
    return slot >= 0 && slot <= 2 && slot_tx(slot).recovery.permits_save();
}

void save_coordinator_on_session_enter(int32_t slot, uint64_t frame, const QolSlotOperationLease* lease) {
    if (!main_thread() || slot < 0 || slot > 2) return;
    std::vector<uint8_t> bytes(1024 * 1024);
    uint64_t size = 0;
    uint16_t version = 0;
    SaveJournal journal;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.backend == nullptr) return;
        const auto read = g.backend->read_section(g.owner, slot, kSaveJournalSection, bytes.data(),
                                                  bytes.size(), &size, &version);
        if (read.code == kQolErrNotFound) return;  // 无未决 journal。
        if (!succeeded(read) ||
            decode_save_journal(version, slot, bytes.data(), size, &journal) != JournalDecode::kValid) {
            slot_tx(slot).recovery.degrade();  // 损坏/v1/未知：保留证据降级。
            return;
        }
        if (journal.stage != JournalStage::kOriginalConfirmed) {
            slot_tx(slot).recovery.degrade();  // prepared 崩溃窗口不自动提交。
            return;
        }
    }
    // stage=1：当前 dat evidence 匹配才自动重试同 tx replace；不匹配 degrade 留证。
    QolSaveFileEvidence current{};
    SlotTx& tx = slot_tx(slot);
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (lease != nullptr) {
            tx.lease = *lease;
        } else {
            tx.lease.size = sizeof(tx.lease);
            const auto acquired = g.operation->acquire(g.operation->host_ctx, g.owner, 1u << slot, &tx.lease);
            if (!succeeded(acquired)) {
                tx.recovery.degrade();
                return;
            }
            tx.lease_held = true;
        }
        QolOwnerStoreCandidate candidate{};
        candidate.size = sizeof(candidate);
        QolOwnerStoreReservation reservation{};
        reservation.size = sizeof(reservation);
        const auto reserved = g.backend->reserve_in_operation(g.owner, &tx.lease, slot, &candidate, 1,
                                                              &reservation);
        if (!succeeded(reserved)) {
            release_credentials(tx);
            tx.recovery.degrade();
            return;
        }
        tx.reservation = reservation;
        tx.reservation_held = true;
    }
    const auto evidence = g.adapter->read_evidence(g.adapter->adapter_ctx, &tx.lease, slot, &current);
    if (!succeeded(evidence) || !may_auto_recover(journal, current)) {
        // 证据不匹配：保留 journal 与降级健康；凭据保留给显式恢复，或直接释放由人工处理。
        // 冻结语义：不自动提升；这里选择释放新签发的凭据并保留证据。
        release_credentials(tx);
        tx.recovery.degrade();
        tx.captured = captured_from_journal(journal);
        tx.journal = journal;
        return;
    }
    tx.captured = captured_from_journal(journal);
    tx.journal = journal;
    tx.tx_id = journal.tx_id;
    tx.phase = TxPhase::kAwaitingReplace;
    tx.frame_retry = true;  // 进档恢复：pending 由生产帧任务推进，不占 HTTP 票据。
    tx.recovery.begin_pending();  // 自动重试窗口：帧驱动推进，不重新 capture。
    (void)frame;
    std::string error_text;
    (void)save_coordinator_retry_frame(slot, frame, &error_text);
}

SaveSettle save_coordinator_retry_frame(int32_t slot, uint64_t frame, std::string* out_error) {
    if (out_error != nullptr) out_error->clear();
    if (!main_thread() || slot < 0 || slot > 2) {
        if (out_error != nullptr) *out_error = "invalid retry";
        return SaveSettle::kFailed;
    }
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.backend == nullptr) {
        if (out_error != nullptr) *out_error = "coordinator not installed";
        return SaveSettle::kFailed;
    }
    SlotTx& tx = slot_tx(slot);
    if (tx.phase == TxPhase::kIdle || !tx.recovery.retry_frame(frame)) {
        // 不在可重试窗口：degraded 或已耗尽。
        if (tx.phase != TxPhase::kIdle && tx.recovery.health() == SlotHealth::kDegraded) {
            finish_capture(tx, value(QolSlotDataOutcome::kDegraded));  // 收敛捕获事务（保留证据）。
            tx.frame_retry = false;
            if (out_error != nullptr) *out_error = "recovery retries exhausted";
            return SaveSettle::kFailed;
        }
        if (out_error != nullptr) *out_error = "no retry scheduled";
        return SaveSettle::kPending;
    }
    if (tx.phase == TxPhase::kAwaitingEvidence) {
        QolSaveFileEvidence current{};
        const auto evidence =
            g.adapter->read_evidence(g.adapter->adapter_ctx, &tx.lease, slot, &current);
        if (!succeeded(evidence)) {
            if (tx.recovery.retry_result(false) == LeaseAction::kRelease) {
                // 不可达（retry_result 成功才 release）；耗尽转 degraded 保留凭据与证据。
                tx.recovery.degrade();
                if (out_error != nullptr) *out_error = "evidence unavailable; degraded";
                return SaveSettle::kFailed;
            }
            return SaveSettle::kPending;
        }
        tx.journal.stage = JournalStage::kOriginalConfirmed;
        tx.journal.evidence = current;
        std::vector<uint8_t> confirmed;
        if (!encode_save_journal(tx.journal, &confirmed)) {
            tx.recovery.degrade();
            if (out_error != nullptr) *out_error = "confirmed journal encode failed; degraded";
            return SaveSettle::kFailed;
        }
        tx.confirmed_bytes = std::move(confirmed);
        tx.phase = TxPhase::kAwaitingConfirmed;
    }
    if (tx.phase == TxPhase::kAwaitingConfirmed) {
        auto credential = tx.reservation;
        const auto written =
            g.backend->write_reserved(g.owner, slot, &credential, kSaveJournalSection,
                                       kSaveJournalVersion, tx.confirmed_bytes.data(), tx.confirmed_bytes.size());
        tx.reservation = credential;
        if (!succeeded(written)) {
            if (tx.recovery.retry_result(false) == LeaseAction::kRelease) {
                tx.recovery.degrade();
                if (out_error != nullptr) *out_error = "confirmed write failed; degraded";
                return SaveSettle::kFailed;
            }
            return SaveSettle::kPending;
        }
        tx.phase = TxPhase::kAwaitingReplace;
    }
    if (tx.phase == TxPhase::kAwaitingReplace) {
        if (tx.confirmed_bytes.empty() && !encode_save_journal(tx.journal, &tx.confirmed_bytes)) {
            tx.recovery.degrade();
            if (out_error != nullptr) *out_error = "confirmed journal encode failed; degraded";
            return SaveSettle::kFailed;
        }
        std::vector<QolStoreSection> sections;
        sections.reserve(tx.captured.size());
        for (const auto& item : tx.captured)
            sections.push_back(QolStoreSection{item.name.c_str(), item.version, item.payload.data(),
                                               static_cast<uint32_t>(item.payload.size())});
        const char* removal = kSaveJournalSection;
        auto credential = tx.reservation;
        const auto replaced = g.backend->replace_reserved(g.owner, slot, &credential,
                                                          sections.data(),
                                                          static_cast<uint32_t>(sections.size()),
                                                          &removal, 1);
        tx.reservation = credential;
        if (!succeeded(replaced)) {
            if (tx.recovery.retry_result(false) == LeaseAction::kRelease) {
                tx.recovery.degrade();
                if (out_error != nullptr) *out_error = "replace failed; degraded";
                return SaveSettle::kFailed;
            }
            return SaveSettle::kPending;
        }
        // 确认提交完成：清 pending 状态 → complete(committed) 清 dirty（按捕获快照），
        // 先 reservation 后 operation 释放。
        (void)tx.recovery.retry_result(true);
        finish_capture(tx, value(QolSlotDataOutcome::kCommitted));
        release_credentials(tx);
        tx.captured.clear();
        tx.confirmed_bytes.clear();
        tx.journal = SaveJournal{};
        tx.phase = TxPhase::kIdle;
        tx.frame_retry = false;
        return SaveSettle::kCommitted;
    }
    if (out_error != nullptr) *out_error = "unexpected phase";
    return SaveSettle::kFailed;
}

SaveSettle save_coordinator_settle(int32_t slot, uint32_t attempt, uint64_t frame,
                                   std::string* out_body) {
    if (out_body != nullptr) out_body->clear();
    if (slot < 0 || slot > 2) {
        if (out_body != nullptr) *out_body = "{\"ok\":false,\"error\":\"bad slot\"}";
        return SaveSettle::kFailed;
    }
    bool has_tx = false;
    bool healthy = true;
    bool frame_retry = false;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        has_tx = slot_tx(slot).phase != TxPhase::kIdle;
        healthy = slot_tx(slot).recovery.permits_save();
        frame_retry = slot_tx(slot).frame_retry;
    }
    // 同槽未决事务：后续 attempt 只查询/推进该 tx（首执行后续 defer）。
    if (has_tx) {
        if (attempt == 0) {
            if (out_body != nullptr) *out_body = "{\"ok\":false,\"error\":\"save in flight\"}";
            return SaveSettle::kFailed;
        }
        // 该 tx 由生产帧任务推进：HTTP 票据只报告 pending，不重复推进。
        if (frame_retry) return SaveSettle::kPending;
        std::string error_text;
        const auto settled = save_coordinator_retry_frame(slot, frame, &error_text);
        if (settled == SaveSettle::kCommitted) {
            if (out_body != nullptr)
                *out_body = "{\"ok\":true,\"slot\":" + std::to_string(slot) +
                            ",\"sidecar\":\"ensured\"}";
            return SaveSettle::kCommitted;
        }
        if (settled == SaveSettle::kPending) return SaveSettle::kPending;
        if (out_body != nullptr && !error_text.empty())
            *out_body = "{\"ok\":false,\"error\":\"" + error_text + "\"}";
        return SaveSettle::kFailed;
    }
    if (attempt != 0) {
        // 前一事务已终态：degraded 的槽拒绝新保存；healthy 时无绑定 tx 的 defer 是异常序列。
        if (!healthy) {
            if (out_body != nullptr) *out_body = "{\"ok\":false,\"error\":\"slot degraded\"}";
            return SaveSettle::kFailed;
        }
        if (out_body != nullptr) *out_body = "{\"ok\":false,\"error\":\"no bound transaction\"}";
        return SaveSettle::kFailed;
    }
    const auto executed = save_execute_impl(slot, frame, false);
    if (executed.settle == SaveSettle::kCommitted) {
        if (out_body != nullptr)
            *out_body = "{\"ok\":true,\"slot\":" + std::to_string(slot) +
                        ",\"sidecar\":\"ensured\"}";
        return SaveSettle::kCommitted;
    }
    if (executed.settle == SaveSettle::kPending) return SaveSettle::kPending;
    if (out_body != nullptr && !executed.error.empty())
        *out_body = "{\"ok\":false,\"error\":\"" + executed.error + "\"}";
    return SaveSettle::kFailed;
}

SaveSettle save_coordinator_save_entry(int32_t slot, uint32_t attempt, uint64_t frame,
                                       contracts::QolSaveOriginalResult* out_original,
                                       std::string* out_body) {
    if (out_body != nullptr) out_body->clear();
    if (out_original != nullptr) {
        out_original->size = sizeof(*out_original);
        out_original->invoked = 0;
        out_original->result_valid = 0;
        out_original->original_result = 0;
    }
    if (slot < 0 || slot > 2) {
        if (out_body != nullptr) *out_body = "{\"ok\":false,\"error\":\"bad slot\"}";
        return SaveSettle::kFailed;
    }
    bool has_tx = false;
    bool healthy = true;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.backend == nullptr) {
            if (out_body != nullptr) *out_body = "{\"ok\":false,\"error\":\"coordinator not installed\"}";
            return SaveSettle::kFailed;
        }
        has_tx = slot_tx(slot).phase != TxPhase::kIdle;
        healthy = slot_tx(slot).recovery.permits_save();
    }
    // 前一次 hook 保存仍 pending：本 invocation 未执行原版，不得透传上一事务的原版返回。
    if (has_tx) {
        if (out_body != nullptr) *out_body = "{\"ok\":false,\"error\":\"save in flight\"}";
        return SaveSettle::kFailed;
    }
    if (attempt != 0) {
        if (!healthy) {
            if (out_body != nullptr) *out_body = "{\"ok\":false,\"error\":\"slot degraded\"}";
            return SaveSettle::kFailed;
        }
        if (out_body != nullptr) *out_body = "{\"ok\":false,\"error\":\"no bound transaction\"}";
        return SaveSettle::kFailed;
    }
    const auto executed = save_execute_impl(slot, frame, true);
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        // 只有本次 invocation 真正执行原版（invoked=1）才透传原版真实 int。
        if (out_original != nullptr && slot_tx(slot).original.invoked != 0)
            *out_original = slot_tx(slot).original;
    }
    if (executed.settle == SaveSettle::kCommitted) {
        if (out_body != nullptr)
            *out_body = "{\"ok\":true,\"slot\":" + std::to_string(slot) +
                        ",\"sidecar\":\"ensured\"}";
        return SaveSettle::kCommitted;
    }
    if (executed.settle == SaveSettle::kPending) return SaveSettle::kPending;
    if (out_body != nullptr && !executed.error.empty())
        *out_body = "{\"ok\":false,\"error\":\"" + executed.error + "\"}";
    return SaveSettle::kFailed;
}

// 生产帧任务驱动 hook 入口发起且仍 pending 的未决事务：≤3 次后续帧、同一 tx、
// 不重新 capture、不重调原版。HTTP 票据绑定的 tx（frame_retry=false）由 attempt 推进，
// 此处跳过以避免重复推进。
void save_coordinator_pump(uint64_t frame) {
    if (!main_thread()) return;
    for (int32_t slot = 0; slot <= 2; ++slot) {
        bool drive = false;
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (g.backend == nullptr) return;
            const SlotTx& tx = slot_tx(slot);
            drive = tx.phase != TxPhase::kIdle && tx.frame_retry &&
                    tx.recovery.health() != SlotHealth::kDegraded;
        }
        if (!drive) continue;
        std::string error_text;
        (void)save_coordinator_retry_frame(slot, frame, &error_text);
    }
}

QolResult save_coordinator_journal_verdict(int32_t slot, const QolSlotOperationLease* lease, void* ctx) {
    (void)ctx;
    // stage=1 的自动恢复（同 tx replace 重试，不重 capture 不重调原版）；恢复后槽健康
    // 决定确认序放行或阻断（degraded 保留证据并阻断）。
    save_coordinator_on_session_enter(slot, 0, lease);
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.backend == nullptr) return ok();  // 协调器未装配：交由保守阻断（上层语义）。
    }
    if (save_coordinator_permits_transition(slot)) return ok();
    return fail(kQolErrBlocked, "journal recovery did not complete");
}
}  // namespace qol::features::core_save
