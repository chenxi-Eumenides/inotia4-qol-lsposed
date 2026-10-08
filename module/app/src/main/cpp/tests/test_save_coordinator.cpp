// P3b-3：联合保存协调器（真 infra store/gate + fake adapter + 真 provider capture 面）。
// 覆盖：capture 恰一次；prepared 失败不调原版；原版 0 不替换+条件清理；原版成功+
// sidecar 失败进 pending（凭据保持）；帧重试不重 capture/不重调原版；耗尽 degraded；
// 重启恢复矩阵（confirmed+match / evidence 不匹配 / prepared / v1 / 损坏）；释放顺序
// reservation→operation；pending 期拒绝新保存；冻结成功体。
#include "contracts/slot_data.h"
#include "host/owner_identity.h"
#include "infra/slot_operation_gate.h"
#include "infra/store.h"
#include "plugins/core.save/save_coordinator.h"
#include "plugins/core.save/save_journal.h"
#include "plugins/core.save/slot_data_provider.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace qol::contracts;
using namespace qol::features::core_save;
static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

namespace {
const auto g_main_thread = std::this_thread::get_id();
int32_t is_main() { return std::this_thread::get_id() == g_main_thread; }

// ---- 真 store 静态表 + 失败注入 wrapper ----
// ABI 表按 QolOwnerStoreApi 字段顺序整体提供；实现经 owner_authorize_store 收口，
// 并把 QolStoreSection 转成 infra 的 ReplaceMap/RemoveSet（与 host_sdk 同构）。
int g_write_reserved_fail = 0;
int g_replace_reserved_fail = 0;
int g_remove_reserved_fail = 0;
int g_last_reserve_candidates = 0;
std::vector<int> g_release_order;  // 0=reservation 1=operation
bool store_authorized(QolOwnerToken token) {
    return succeeded(qol::host::owner_authorize_store(token, "module-saves"));
}
QolResult from_status(qol::infra::store::Status status, const char* message) {
    if (status == qol::infra::store::Status::kOk) return ok();
    const int32_t code = status == qol::infra::store::Status::kNotFound ? kQolErrNotFound
                         : status == qol::infra::store::Status::kBusy   ? kQolErrBusy
                         : status == qol::infra::store::Status::kBlocked ? kQolErrBlocked
                         : status == qol::infra::store::Status::kUnavailable ? kQolErrNotReady
                         : status == qol::infra::store::Status::kBadName ||
                                   status == qol::infra::store::Status::kBadSlot ||
                                   status == qol::infra::store::Status::kBadVersion
                             ? kQolErrInvalidArgument
                             : kQolErrInternal;
    return error(code, message, static_cast<uint32_t>(std::strlen(message)));
}
QolResult store_ensure(QolOwnerToken token, int32_t slot) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    return from_status(qol::infra::store::ensure_slot(slot), "ensure failed");
}
QolResult store_reset(QolOwnerToken token, int32_t slot) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    return from_status(qol::infra::store::reset_slot(slot), "reset failed");
}
QolResult store_read_section(QolOwnerToken token, int32_t slot, const char* name, uint8_t* out,
                             uint64_t capacity, uint64_t* size, uint16_t* version) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    std::vector<uint8_t> bytes;
    uint16_t stored = 0;
    const auto status = qol::infra::store::read_section(slot, name, &bytes, &stored);
    if (status != qol::infra::store::Status::kOk) return from_status(status, "read failed");
    if (bytes.size() > capacity) return error(kQolErrInvalidArgument, "large", 5);
    if (!bytes.empty()) std::memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size();
    *version = stored;
    return ok();
}
QolResult store_write_section(QolOwnerToken token, int32_t slot, const char* name,
                              uint16_t version, const uint8_t* bytes, uint64_t size) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    return from_status(qol::infra::store::write_section(slot, name, version, bytes, size),
                       "write failed");
}
QolResult store_remove_section(QolOwnerToken token, int32_t slot, const char* name) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    return from_status(qol::infra::store::remove_section(slot, name), "remove failed");
}
QolResult store_replace_sections(QolOwnerToken token, int32_t slot,
                                 const QolStoreSection* sections, uint32_t count,
                                 const char* const* removals, uint32_t removal_count) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    qol::infra::store::ReplaceMap replacements;
    for (uint32_t i = 0; i < count; ++i) {
        qol::infra::store::Section section;
        section.version = sections[i].version;
        if (sections[i].size != 0)
            section.payload.assign(sections[i].data, sections[i].data + sections[i].size);
        replacements.emplace_back(sections[i].name != nullptr ? sections[i].name : "",
                                  std::move(section));
    }
    qol::infra::store::RemoveSet removed;
    for (uint32_t i = 0; i < removal_count; ++i)
        removed.emplace_back(removals[i] != nullptr ? removals[i] : "");
    return from_status(
        qol::infra::store::replace_sections(slot, replacements, removed), "replace failed");
}
QolResult store_preflight(QolOwnerToken token, int32_t slot,
                          const QolOwnerStoreCandidate* candidates, uint32_t count,
                          uint64_t* lengths) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    return from_status(qol::infra::store::preflight(slot, candidates, count, lengths),
                       "preflight failed");
}
QolResult store_reserve(QolOwnerToken token, int32_t slot,
                        const QolOwnerStoreCandidate* candidates, uint32_t count,
                        QolOwnerStoreReservation* out) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    return from_status(qol::infra::store::reserve(token, slot, candidates, count, out),
                       "reserve failed");
}
QolResult store_release(QolOwnerToken token, QolOwnerStoreReservation reservation) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    const auto status = qol::infra::store::release(token, reservation);
    if (status == qol::infra::store::Status::kOk) g_release_order.push_back(0);
    return from_status(status, "release failed");
}
QolResult store_write_reserved(QolOwnerToken token, int32_t slot,
                               QolOwnerStoreReservation* credential, const char* name,
                               uint16_t version, const uint8_t* bytes, uint64_t size) {
    if (g_write_reserved_fail > 0) {
        --g_write_reserved_fail;
        return error(kQolErrInternal, "write reserved injected failure", 32);
    }
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    qol::infra::store::ReplaceMap replacements;
    qol::infra::store::Section section;
    section.version = version;
    if (size != 0) section.payload.assign(bytes, bytes + size);
    replacements.emplace_back(name != nullptr ? name : "", std::move(section));
    return from_status(qol::infra::store::replace_reserved(token, slot, credential, replacements, {}),
                       "write reserved failed");
}
QolResult store_remove_reserved(QolOwnerToken token, int32_t slot,
                                QolOwnerStoreReservation* credential, const char* name) {
    if (g_remove_reserved_fail > 0) {
        --g_remove_reserved_fail;
        return error(kQolErrInternal, "remove reserved injected failure", 33);
    }
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    return from_status(
        qol::infra::store::replace_reserved(token, slot, credential, {},
                                            qol::infra::store::RemoveSet{name != nullptr ? name : ""}),
        "remove reserved failed");
}
QolResult store_replace_reserved(QolOwnerToken token, int32_t slot,
                                 QolOwnerStoreReservation* credential,
                                 const QolStoreSection* sections, uint32_t count,
                                 const char* const* removals, uint32_t removal_count) {
    if (g_replace_reserved_fail > 0) {
        --g_replace_reserved_fail;
        return error(kQolErrInternal, "replace reserved injected failure", 33);
    }
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    qol::infra::store::ReplaceMap replacements;
    for (uint32_t i = 0; i < count; ++i) {
        qol::infra::store::Section section;
        section.version = sections[i].version;
        if (sections[i].size != 0)
            section.payload.assign(sections[i].data, sections[i].data + sections[i].size);
        replacements.emplace_back(sections[i].name != nullptr ? sections[i].name : "",
                                  std::move(section));
    }
    qol::infra::store::RemoveSet removed;
    for (uint32_t i = 0; i < removal_count; ++i)
        removed.emplace_back(removals[i] != nullptr ? removals[i] : "");
    return from_status(
        qol::infra::store::replace_reserved(token, slot, credential, replacements, removed),
        "replace reserved failed");
}
QolResult store_set_referenced(QolOwnerToken token, int32_t slot,
                               QolOwnerStoreReservation* credential, uint32_t referenced) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    return from_status(qol::infra::store::set_referenced(token, slot, credential, referenced),
                       "set_referenced failed");
}
QolResult store_reserve_in_operation(QolOwnerToken token, const QolSlotOperationLease* lease,
                                     int32_t slot, const QolOwnerStoreCandidate* candidates,
                                     uint32_t count, QolOwnerStoreReservation* out) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    g_last_reserve_candidates = static_cast<int>(count);
    return from_status(
        qol::infra::store::reserve_in_operation(token, lease, slot, candidates, count, out),
        "reserve in operation failed");
}
QolResult store_ensure_in_operation(QolOwnerToken token, const QolSlotOperationLease* lease,
                                    int32_t slot) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    return from_status(qol::infra::store::ensure_in_operation(token, lease, slot),
                       "ensure in operation failed");
}
QolResult store_reset_in_operation(QolOwnerToken token, const QolSlotOperationLease* lease,
                                   int32_t slot) {
    if (!store_authorized(token)) return error(kQolErrBlocked, "unauthorized", 12);
    return from_status(qol::infra::store::reset_in_operation(token, lease, slot),
                       "reset in operation failed");
}
const QolOwnerStoreApi kBackend{{sizeof(QolOwnerStoreApi), 1, 0},
                                &store_ensure, &store_reset, &store_read_section,
                                &store_write_section, &store_remove_section,
                                &store_replace_sections, nullptr, &store_preflight, &store_reserve,
                                &store_release, &store_write_reserved, &store_remove_reserved,
                                &store_replace_reserved, &store_set_referenced,
                                &store_reserve_in_operation, &store_ensure_in_operation,
                                &store_reset_in_operation};

// ---- operation 表（真 gate）----
QolResult op_acquire(void*, QolOwnerToken owner, uint32_t mask, QolSlotOperationLease* out) {
    if (!succeeded(qol::host::owner_authorize_slot_operation(owner)))
        return error(kQolErrBlocked, "not authorized", 13);
    return qol::infra::slot_operation::acquire(owner, mask, out);
}
QolResult op_set_referenced(void*, QolOwnerToken owner, QolSlotOperationLease* lease,
                            uint32_t referenced) {
    if (!succeeded(qol::host::owner_authorize_slot_operation(owner)))
        return error(kQolErrBlocked, "not authorized", 13);
    return qol::infra::slot_operation::set_referenced(owner, lease, referenced);
}
QolResult op_release(void*, QolOwnerToken owner, const QolSlotOperationLease* lease) {
    if (!succeeded(qol::host::owner_authorize_slot_operation(owner)))
        return error(kQolErrBlocked, "not authorized", 13);
    const auto result = qol::infra::slot_operation::release(owner, lease);
    if (succeeded(result)) g_release_order.push_back(1);
    return result;
}
const QolSlotOperationApi kOperation{{sizeof(QolSlotOperationApi), 1, 0}, nullptr, &op_acquire,
                                     &op_set_referenced, nullptr, &op_release, nullptr, nullptr};

// ---- fake adapter ----
struct AdapterState {
    int call_count = 0;
    int32_t original_result = 1;
    bool fail_call = false;
    bool evidence_fail = 0;
    QolSaveFileEvidence evidence{7, {1, 2, 3, 4}};
} g_adapter;
QolResult adapter_call(void*, const QolSlotOperationLease*, int32_t, QolSaveOriginalResult* out) {
    ++g_adapter.call_count;
    out->size = sizeof(*out);
    if (g_adapter.fail_call) return error(kQolErrNotReady, "adapter not ready", 17);
    out->invoked = 1;
    out->result_valid = 1;
    out->original_result = g_adapter.original_result;
    return ok();
}
QolResult adapter_read_evidence(void*, const QolSlotOperationLease*, int32_t,
                                QolSaveFileEvidence* out) {
    if (g_adapter.evidence_fail) return error(kQolErrInternal, "no evidence", 11);
    *out = g_adapter.evidence;
    return ok();
}
const QolSaveTransactionPort kAdapter{{sizeof(QolSaveTransactionPort), 1, 0}, nullptr, nullptr,
                                      nullptr, &adapter_call, nullptr, &adapter_read_evidence};

// ---- provider 参与者 ----
struct Consumer {
    int captures = 0;
    int completes = 0;
    std::vector<uint32_t> outcomes;
    QolSlotDataHandle handle = 0;
};
QolResult on_detach(void*, const QolSlotSession*, uint32_t) { return ok(); }
QolResult on_ready(void*, const QolSlotDataContext*) { return ok(); }
QolResult on_load(void*, const QolSlotDataContext*, uint32_t, uint32_t, const uint8_t*, uint64_t) {
    return ok();
}
int g_capture_calls = 0;
QolResult on_capture(void* user, const QolSlotDataContext*, uint8_t* out, uint64_t, uint64_t* size) {
    ++g_capture_calls;
    ++static_cast<Consumer*>(user)->captures;
    out[0] = 42;
    out[1] = 7;
    *size = 2;
    return ok();
}
void on_complete(void* user, const QolSlotDataContext*, uint32_t outcome) {
    auto& c = *static_cast<Consumer*>(user);
    ++c.completes;
    c.outcomes.push_back(outcome);
}
QolSlotDataSpec consumer_spec(QolOwnerToken token, Consumer* c) {
    return {sizeof(QolSlotDataSpec), token, "coordinator.data", 1,
            static_cast<uint32_t>(QolSlotDataReadPolicy::kSessionBound),
            static_cast<uint32_t>(QolSlotDataWritePolicy::kSlotBound),
            c, &on_detach, &on_ready, &on_load, &on_capture, &on_complete};
}

bool make_session_ready() {
    if (!succeeded(slot_data_begin_transition(static_cast<uint32_t>(QolSlotDataReason::kEnter), 0)))
        return false;
    return succeeded(slot_data_confirm_transition(0));
}
QolSlotSession session_of() {
    QolSlotSession out{-1, 0, 0};
    CHECK(succeeded(slot_data_api().session(&out)));
    return out;
}
bool journal_on_disk_missing() {
    std::vector<uint8_t> bytes;
    uint16_t version = 0;
    return qol::infra::store::read_section(0, "module.save.journal", &bytes, &version) ==
           qol::infra::store::Status::kNotFound;
}
void remove_tree(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir != nullptr) {
        while (dirent* ent = ::readdir(dir)) {
            const std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = path + "/" + name;
            struct stat st{};
            if (::lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(full);
            else ::unlink(full.c_str());
        }
        ::closedir(dir);
    }
    ::rmdir(path.c_str());
}
}  // namespace

int main() {
    char dir[] = "save_coordinator_XXXXXX";
    CHECK(::mkdtemp(dir) != nullptr);
    const std::string root = dir;
    qol::infra::store::init(root.c_str());

    int provider_identity = 0, consumer_identity = 0, stranger_identity = 0;
    CHECK(qol::host::owner_register(&provider_identity));
    CHECK(qol::host::owner_register(&consumer_identity));
    CHECK(qol::host::owner_register(&stranger_identity));
    CHECK(qol::host::owner_bind_store(&provider_identity, "module-saves"));
    CHECK(qol::host::owner_bind_slot_operation(&provider_identity));
    CHECK(qol::host::owner_bind_slot_operation(&stranger_identity));
    auto& identities = qol::host::owner_identity_api();
    QolOwnerToken provider = 0, consumer = 0, stranger = 0;
    uint64_t generation = 0;
    CHECK(succeeded(identities.query(&provider_identity, &provider, &generation)));
    CHECK(succeeded(identities.query(&consumer_identity, &consumer, &generation)));
    CHECK(succeeded(identities.query(&stranger_identity, &stranger, &generation)));

    CHECK(succeeded(slot_data_initialize(provider, &qol::host::owner_verify_token, &kBackend, &is_main)));
    slot_data_set_available(true);
    CHECK(succeeded(save_coordinator_initialize(provider, &qol::host::owner_verify_token, &kBackend,
                                                &kOperation, &kAdapter, &is_main)));
    CHECK(save_coordinator_installed());

    Consumer consumer_state;
    QolSlotDataRequest initial = 0;
    auto spec = consumer_spec(consumer, &consumer_state);
    CHECK(succeeded(slot_data_api().register_section(&spec, &consumer_state.handle, &initial)));
    CHECK(make_session_ready());
    // 置 dirty：SlotBound 写请求标记，等协调器 committed 清除。
    QolSlotDataRequest request = 0;
    CHECK(succeeded(slot_data_api().request_write(consumer_state.handle, session_of().epoch, &request)));

    // ---- 全链 committed：冻结成功体、capture 恰一次、journal 删除、释放顺序 ----
    g_capture_calls = 0;
    std::string body;
    g_adapter.original_result = 42;
    QolSaveOriginalResult original{};
    CHECK(save_coordinator_save_entry(0, 0, 1, &original, &body) == SaveSettle::kCommitted);
    CHECK(body == "{\"ok\":true,\"slot\":0,\"sidecar\":\"ensured\"}");
    // SAVE_Save 入口透传：游戏可见 int = 原版真实返回（invoked=1，result_valid=1）。
    CHECK(original.size == sizeof(original) && original.invoked == 1 &&
          original.result_valid == 1 && original.original_result == 42);
    g_adapter.original_result = 1;
    CHECK(g_capture_calls == 1 && consumer_state.captures == 1);
    // B5：prepared/confirmed/final 三候选基于同一容器一次预约。
    CHECK(g_last_reserve_candidates == 3);
    // request_write（SlotBound）会先 complete(kStaged)；committed 终态追加 kCommitted。
    CHECK(consumer_state.outcomes.back() ==
          static_cast<uint32_t>(QolSlotDataOutcome::kCommitted));
    CHECK(journal_on_disk_missing());
    std::vector<uint8_t> committed_bytes;
    uint16_t committed_version = 0;
    CHECK(qol::infra::store::read_section(0, "coordinator.data", &committed_bytes,
                                          &committed_version) ==
          qol::infra::store::Status::kOk);
    const std::vector<uint8_t> expected_payload{42, 7};
    CHECK(committed_bytes == expected_payload && committed_version == 1);
    CHECK(g_release_order.size() >= 2);
    const auto last_two = std::vector<int>(g_release_order.end() - 2, g_release_order.end());
    const std::vector<int> expected_order{0, 1};
    CHECK(last_two == expected_order);  // 先 reservation 后 operation。

    // ---- prepared 写失败：不调原版、complete(cancelled)、双凭据释放 ----
    const auto calls_before = g_adapter.call_count;
    const auto captures_before = g_capture_calls;
    g_write_reserved_fail = 1;
    CHECK(save_coordinator_settle(0, 0, 2, &body) == SaveSettle::kFailed);
    CHECK(body.find("prepared journal write failed") != std::string::npos);
    CHECK(g_adapter.call_count == calls_before);  // 不调用原版。
    CHECK(g_capture_calls == captures_before + 1);  // capture 已执行恰一次。
    CHECK(consumer_state.outcomes.back() == static_cast<uint32_t>(QolSlotDataOutcome::kCancelled));
    CHECK(journal_on_disk_missing());
    QolSlotOperationLease probe{};
    probe.size = sizeof(probe);
    CHECK(succeeded(qol::infra::slot_operation::acquire(stranger, 1, &probe)));  // lease 已释放。
    CHECK(succeeded(qol::infra::slot_operation::release(stranger, &probe)));

    // ---- 原版有效返回 0：不替换正式 sections、条件清理 journal、original-save-failed ----
    std::vector<uint8_t> seed{1, 2, 3};
    CHECK(qol::infra::store::write_section(0, "seed.section", 1, seed.data(), seed.size()) ==
          qol::infra::store::Status::kOk);
    g_adapter.original_result = 0;
    CHECK(save_coordinator_settle(0, 0, 3, &body) == SaveSettle::kFailed);
    CHECK(body.find("original save failed") != std::string::npos);
    std::vector<uint8_t> seed_after;
    uint16_t seed_version = 0;
    CHECK(qol::infra::store::read_section(0, "seed.section", &seed_after, &seed_version) ==
          qol::infra::store::Status::kOk);
    CHECK(seed_after == seed);  // 正式 sections 未被捕获字节替换。
    CHECK(journal_on_disk_missing());  // 身份匹配的 prepared journal 已清理。
    CHECK(consumer_state.outcomes.back() ==
          static_cast<uint32_t>(QolSlotDataOutcome::kOriginalSaveFailed));
    g_adapter.original_result = 1;

    // ---- 原版 0 但 journal 清理失败（条件清理不成立）：保留证据转 degraded ----
    g_adapter.original_result = 0;
    g_remove_reserved_fail = 1;  // 条件清理（remove_reserved）失败。
    CHECK(save_coordinator_settle(0, 0, 4, &body) == SaveSettle::kFailed);
    CHECK(body.find("journal preserved") != std::string::npos);
    CHECK(consumer_state.outcomes.back() == static_cast<uint32_t>(QolSlotDataOutcome::kDegraded));
    CHECK(!save_coordinator_permits_save(0));  // degraded 拒绝新保存。
    QolSaveOriginalResult rejected_original{};
    CHECK(save_coordinator_save_entry(0, 0, 5, &rejected_original, &body) == SaveSettle::kFailed);
    CHECK(rejected_original.invoked == 0);  // R1：健康拒绝不能泄漏上一事务的 original。
    CHECK(qol::infra::store::read_section(0, "module.save.journal", &seed_after,
                                          &seed_version) == qol::infra::store::Status::kOk);
    save_coordinator_reset_for_test();  // 先释放 degraded 保留的凭据，再清 journal 证据。
    CHECK(qol::infra::store::remove_section(0, "module.save.journal") ==
          qol::infra::store::Status::kOk);
    g_adapter.original_result = 1;

    // ---- 原版成功 + final replace 阻断：commit-pending；重试不重 capture/不重调原版 ----
    const auto captures_at_pending = g_capture_calls;
    const auto calls_at_pending = g_adapter.call_count;
    g_replace_reserved_fail = 1;  // 仅 final replace 失败一次；prepared/confirmed 写正常。
    CHECK(save_coordinator_settle(0, 0, 5, &body) == SaveSettle::kPending);
    CHECK(body.empty());
    CHECK(consumer_state.outcomes.back() ==
          static_cast<uint32_t>(QolSlotDataOutcome::kPendingRecovery));
    CHECK(g_adapter.call_count == calls_at_pending + 1);  // 原版只调一次。
    // 凭据保持：lease 被协调器持有，他人 acquire Busy。
    CHECK(qol::infra::slot_operation::acquire(stranger, 1, &probe).code == kQolErrBusy);
    // pending 期拒绝新保存（新事务入口）。
    CHECK(save_coordinator_settle(0, 0, 6, &body) == SaveSettle::kFailed);
    CHECK(body.find("save in flight") != std::string::npos);
    // 下一帧重试成功：不重 capture、不重调原版。
    CHECK(save_coordinator_settle(0, 1, 7, &body) == SaveSettle::kCommitted);
    CHECK(body == "{\"ok\":true,\"slot\":0,\"sidecar\":\"ensured\"}");
    CHECK(g_capture_calls == captures_at_pending + 1);  // 仅首次事务 capture 一次，重试不重 capture。
    CHECK(g_adapter.call_count == calls_at_pending + 1);
    CHECK(journal_on_disk_missing());

    // ---- B3/B4：hook 入口透传原版真实 int；pending 由帧任务推进（同 tx，不重 capture/不重调原版）----
    g_replace_reserved_fail = 1;  // final replace 首次失败 → hook 事务进入 pending。
    QolSaveOriginalResult hook_original{};
    const auto calls_hook = g_adapter.call_count;
    const auto captures_hook = g_capture_calls;
    CHECK(save_coordinator_save_entry(0, 0, 30, &hook_original, &body) == SaveSettle::kPending);
    CHECK(hook_original.invoked == 1 && hook_original.result_valid == 1 &&
          hook_original.original_result == 1);  // B3：hook 透传原版真实返回，而非 invoked=0/0。
    CHECK(g_adapter.call_count == calls_hook + 1);
    CHECK(g_capture_calls == captures_hook + 1);
    // B5：confirmed journal 携带捕获条目（owner/name/version/payload），重启可恢复。
    {
        std::vector<uint8_t> jbytes;
        uint16_t jver = 0;
        CHECK(qol::infra::store::read_section(0, "module.save.journal", &jbytes, &jver) ==
              qol::infra::store::Status::kOk);
        SaveJournal decoded;
        CHECK(decode_save_journal(jver, 0, jbytes.data(), jbytes.size(), &decoded) ==
              JournalDecode::kValid);
        CHECK(decoded.stage == JournalStage::kOriginalConfirmed);
        CHECK(decoded.entries.size() == 1);
        CHECK(decoded.entries[0].owner_id == consumer);
        CHECK(decoded.entries[0].name == "coordinator.data");
        CHECK(decoded.entries[0].payload == std::vector<uint8_t>({42, 7}));
    }
    // HTTP 票据不重复推进 frame_retry tx：只报告 pending，不消费重试预算。
    CHECK(save_coordinator_settle(0, 1, 30, &body) == SaveSettle::kPending);
    // 生产帧任务推进同一 tx：成功且不重 capture、不重调原版。
    save_coordinator_pump(31);
    CHECK(save_coordinator_permits_save(0));
    CHECK(g_adapter.call_count == calls_hook + 1);
    CHECK(g_capture_calls == captures_hook + 1);
    CHECK(journal_on_disk_missing());
    g_replace_reserved_fail = 0;

    // ---- 重试耗尽：连续 3 帧失败 → degraded 失败终态 ----
    g_replace_reserved_fail = 10;
    const auto captures_before_exhaust = g_capture_calls;
    CHECK(save_coordinator_settle(0, 0, 8, &body) == SaveSettle::kPending);
    CHECK(save_coordinator_settle(0, 1, 9, &body) == SaveSettle::kPending);
    CHECK(save_coordinator_settle(0, 1, 10, &body) == SaveSettle::kPending);
    CHECK(save_coordinator_settle(0, 1, 11, &body) == SaveSettle::kFailed);
    CHECK(body.find("degraded") != std::string::npos);
    CHECK(g_capture_calls == captures_before_exhaust + 1);  // 首次事务 capture 一次，重试不重 capture。
    CHECK(!save_coordinator_permits_save(0) && !save_coordinator_permits_transition(0));
    CHECK(consumer_state.outcomes.back() == static_cast<uint32_t>(QolSlotDataOutcome::kDegraded));
    CHECK(consumer_state.completes >= 1);  // R2：degraded 同次终结捕获引用，凭据仍受保护。
    CHECK(qol::infra::slot_operation::acquire(stranger, 1, &probe).code == kQolErrBusy);
    g_replace_reserved_fail = 0;
    save_coordinator_reset_for_test();  // 清 degraded 继续后续场景。

    // ---- 原版成功 + evidence 缺失 → pending；恢复后同 tx 提交 ----
    const auto calls_at_evidence = g_adapter.call_count;
    g_adapter.evidence_fail = 1;
    CHECK(save_coordinator_settle(0, 0, 11, &body) == SaveSettle::kPending);
    CHECK(g_adapter.call_count == calls_at_evidence + 1);
    g_adapter.evidence_fail = 0;
    CHECK(save_coordinator_settle(0, 1, 12, &body) == SaveSettle::kCommitted);
    CHECK(body == "{\"ok\":true,\"slot\":0,\"sidecar\":\"ensured\"}");
    CHECK(g_adapter.call_count == calls_at_evidence + 1);  // 重试不重调原版。
    CHECK(journal_on_disk_missing());

    // ---- Adapter 层失败（未执行原版）：cancelled + 条件清理 ----
    const auto captures_before_adapter_fail = g_capture_calls;
    g_adapter.fail_call = true;
    CHECK(save_coordinator_settle(0, 0, 13, &body) == SaveSettle::kFailed);
    CHECK(g_capture_calls == captures_before_adapter_fail + 1);
    CHECK(consumer_state.outcomes.back() == static_cast<uint32_t>(QolSlotDataOutcome::kCancelled));
    CHECK(journal_on_disk_missing());
    g_adapter.fail_call = false;

    // ---- 重启恢复矩阵（直接写盘 journal + on_session_enter）----
    std::vector<uint8_t> recovered{5, 5};
    // a) stage1 + evidence 匹配 → 自动重试同 tx replace。
    SaveJournal confirmed{321, 1, 0, JournalStage::kOriginalConfirmed, g_adapter.evidence, {}};
    confirmed.entries.push_back(JournalEntry{provider, "recovered.data", 1, recovered});
    std::vector<uint8_t> confirmed_bytes;
    CHECK(encode_save_journal(confirmed, &confirmed_bytes));
    CHECK(qol::infra::store::write_section(1, "module.save.journal", 2, confirmed_bytes.data(),
                                           confirmed_bytes.size()) ==
          qol::infra::store::Status::kOk);
    save_coordinator_on_session_enter(1, 20);
    CHECK(save_coordinator_permits_save(1));
    std::vector<uint8_t> recovered_disk;
    uint16_t recovered_version = 0;
    // journal 删除断言：slot1 的 journal 应被 replace 移除。
    {
        std::vector<uint8_t> journal_probe;
        uint16_t journal_version = 0;
        CHECK(qol::infra::store::read_section(1, "module.save.journal", &journal_probe,
                                              &journal_version) ==
              qol::infra::store::Status::kNotFound);
    }
    CHECK(qol::infra::store::read_section(1, "recovered.data", &recovered_disk,
                                          &recovered_version) ==
          qol::infra::store::Status::kOk);
    CHECK(recovered_disk == recovered && recovered_version == 1);

    // b) stage1 + evidence 不匹配 → degraded 保留。
    SaveJournal mismatched{322, 1, 0, JournalStage::kOriginalConfirmed, {9, {9, 9}}, {}};
    mismatched.entries.push_back(JournalEntry{provider, "recovered.data", 1, recovered});
    std::vector<uint8_t> mismatched_bytes;
    CHECK(encode_save_journal(mismatched, &mismatched_bytes));
    CHECK(qol::infra::store::write_section(1, "module.save.journal", 2, mismatched_bytes.data(),
                                           mismatched_bytes.size()) ==
          qol::infra::store::Status::kOk);
    save_coordinator_on_session_enter(1, 21);
    CHECK(!save_coordinator_permits_save(1));
    {
        std::vector<uint8_t> journal_probe;
        uint16_t journal_version = 0;
        CHECK(qol::infra::store::read_section(1, "module.save.journal", &journal_probe,
                                              &journal_version) ==
              qol::infra::store::Status::kOk);  // 证据保留。
    }
    save_coordinator_reset_for_test();
    CHECK(qol::infra::store::remove_section(1, "module.save.journal") ==
          qol::infra::store::Status::kOk);

    // c) prepared 崩溃窗口 → degraded 保留。
    SaveJournal prepared{323, 1, 0, JournalStage::kPrepared, {}, {}};
    std::vector<uint8_t> prepared_bytes;
    CHECK(encode_save_journal(prepared, &prepared_bytes));
    CHECK(qol::infra::store::write_section(1, "module.save.journal", 2, prepared_bytes.data(),
                                           prepared_bytes.size()) ==
          qol::infra::store::Status::kOk);
    save_coordinator_on_session_enter(1, 22);
    CHECK(!save_coordinator_permits_save(1));
    save_coordinator_reset_for_test();
    CHECK(qol::infra::store::remove_section(1, "module.save.journal") ==
          qol::infra::store::Status::kOk);

    // d) v1 legacy → degraded 保留。
    const char* v1 =
        "{\"transactionId\":\"legacy-tx\",\"slot\":1,\"stage\":0,\"sections\":"
        "[{\"name\":\"legacy.data\",\"version\":1,\"payload\":\"AQ==\"}]}";
    CHECK(qol::infra::store::write_section(1, "module.save.journal", 1,
                                           reinterpret_cast<const uint8_t*>(v1),
                                           std::strlen(v1)) == qol::infra::store::Status::kOk);
    save_coordinator_on_session_enter(1, 23);
    CHECK(!save_coordinator_permits_save(1));
    save_coordinator_reset_for_test();
    CHECK(qol::infra::store::remove_section(1, "module.save.journal") ==
          qol::infra::store::Status::kOk);

    // e) 损坏字节 → degraded 保留。
    const std::vector<uint8_t> corrupt{0x53, 0x44, 0x4a, 0x32, 0x00};
    CHECK(qol::infra::store::write_section(1, "module.save.journal", 2, corrupt.data(),
                                           corrupt.size()) == qol::infra::store::Status::kOk);
    save_coordinator_on_session_enter(1, 24);
    CHECK(!save_coordinator_permits_save(1));
    save_coordinator_reset_for_test();
    CHECK(qol::infra::store::remove_section(1, "module.save.journal") ==
          qol::infra::store::Status::kOk);

    // ---- B12：合法 ready session 下零参与者保存仍执行原版并提交（不因无模块数据失败）----
    QolSlotDataRequest detach_request = 0;
    CHECK(succeeded(slot_data_api().unregister(consumer_state.handle, &detach_request)));
    CHECK(make_session_ready());
    const auto calls_zero = g_adapter.call_count;
    g_adapter.original_result = 7;
    QolSaveOriginalResult zero_original{};
    CHECK(save_coordinator_save_entry(2, 0, 60, &zero_original, &body) == SaveSettle::kCommitted);
    CHECK(g_adapter.call_count == calls_zero + 1);
    CHECK(zero_original.invoked == 1 && zero_original.result_valid == 1 &&
          zero_original.original_result == 7);
    g_adapter.original_result = 1;

    slot_data_set_available(false);
    save_coordinator_shutdown();
    remove_tree(root);
    std::printf("save_coordinator: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
