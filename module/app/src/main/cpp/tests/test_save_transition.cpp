// P3b-3：core.save 转换状态机（fake backend + 真 operation gate）。
// 覆盖：begin 原子两槽 lease、prepare_detach 锁外拒绝、确认序成功/失败不消费、
// 观测槽校验、token id 显式复用（禁止槽相同复用）、同槽重入新 epoch、取消不恢复 dat、
// journal 严格分派（stage0 恒阻断 / stage1 经协调器 verdict）、prepare_stop。
#include "contracts/slot_data.h"
#include "host/owner_identity.h"
#include "infra/slot_operation_gate.h"
#include "plugins/core.save/save_journal.h"
#include "plugins/core.save/save_transition.h"
#include "plugins/core.save/slot_data_provider.h"

#include <cstdio>
#include <cstring>
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

// ---- fake backend：ensure/reset 可注入失败，read_section 注入 journal ----
int g_ensure_fail = 0;
int g_reset_calls = 0;
int g_ensure_calls = 0;
std::vector<uint8_t> g_journal_bytes;
uint16_t g_journal_version = 2;
QolResult backend_reset(QolOwnerToken token, int32_t slot) {
    ++g_reset_calls;
    if (g_ensure_fail) return error(kQolErrInternal, "reset failed", 11);
    if (!succeeded(qol::host::owner_authorize_store(token, "module-saves")))
        return error(kQolErrBlocked, "unauthorized", 12);
    (void)slot;
    return ok();
}
QolResult backend_ensure(QolOwnerToken token, int32_t slot) {
    ++g_ensure_calls;
    if (g_ensure_fail) return error(kQolErrInternal, "ensure failed", 13);
    if (!succeeded(qol::host::owner_authorize_store(token, "module-saves")))
        return error(kQolErrBlocked, "unauthorized", 12);
    (void)slot;
    return ok();
}
QolResult backend_read_section(QolOwnerToken token, int32_t slot, const char* name, uint8_t* out,
                               uint64_t capacity, uint64_t* size, uint16_t* version) {
    if (!succeeded(qol::host::owner_authorize_store(token, "module-saves")))
        return error(kQolErrBlocked, "unauthorized", 12);
    if (std::strcmp(name, "module.save.journal") != 0 || g_journal_bytes.empty())
        return error(kQolErrNotFound, "missing", 7);
    (void)slot;
    if (g_journal_bytes.size() > capacity) return error(kQolErrInvalidArgument, "large", 5);
    std::memcpy(out, g_journal_bytes.data(), g_journal_bytes.size());
    *size = g_journal_bytes.size();
    *version = g_journal_version;
    return ok();
}
QolResult backend_write_stub(QolOwnerToken, int32_t, const char*, uint16_t, const uint8_t*,
                             uint64_t) {
    return ok();
}
const QolOwnerStoreApi kBackend{{sizeof(QolOwnerStoreApi), 1, 0}, &backend_ensure, &backend_reset,
                                &backend_read_section, &backend_write_stub, nullptr, nullptr, nullptr,
                                nullptr, nullptr, nullptr};

// ---- provider 参与者 ----
struct Consumer {
    int ready = 0, loads = 0, captures = 0, completes = 0, detaches = 0;
    QolSlotDataHandle handle = 0;
};
QolResult on_detach(void* user, const QolSlotSession*, uint32_t) {
    ++static_cast<Consumer*>(user)->detaches;
    return ok();
}
QolResult on_ready(void* user, const QolSlotDataContext*) {
    ++static_cast<Consumer*>(user)->ready;
    return ok();
}
QolResult on_load(void* user, const QolSlotDataContext*, uint32_t, uint32_t, const uint8_t*,
                  uint64_t) {
    ++static_cast<Consumer*>(user)->loads;
    return ok();
}
QolResult on_capture(void* user, const QolSlotDataContext*, uint8_t* out, uint64_t, uint64_t* size) {
    ++static_cast<Consumer*>(user)->captures;
    out[0] = 1;
    *size = 1;
    return ok();
}
void on_complete(void* user, const QolSlotDataContext*, uint32_t) {
    ++static_cast<Consumer*>(user)->completes;
}
QolSlotDataSpec consumer_spec(QolOwnerToken token, Consumer* c) {
    return {sizeof(QolSlotDataSpec), token, "transition.data", 1,
            static_cast<uint32_t>(QolSlotDataReadPolicy::kSessionBound),
            static_cast<uint32_t>(QolSlotDataWritePolicy::kSlotBound),
            c, &on_detach, &on_ready, &on_load, &on_capture, &on_complete};
}

int g_verdict_slot = -1;
QolResult allow_verdict(int32_t slot, void*) {
    g_verdict_slot = slot;
    return ok();
}

// operation 表：authorize（owner_identity）+ 真 gate（与 host_sdk 同构）。
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
    return qol::infra::slot_operation::release(owner, lease);
}
const QolSlotOperationApi kOperation{{sizeof(QolSlotOperationApi), 1, 0}, nullptr, &op_acquire,
                                     &op_set_referenced, nullptr, &op_release, nullptr, nullptr};
}  // namespace

int main() {
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
    CHECK(succeeded(save_transition_initialize(provider, &qol::host::owner_verify_token, &kBackend,
                                               &kOperation, &is_main)));
    CHECK(!save_transition_ready_for_save_enter());

    // ---- begin：无旧 session，原子取得单槽 lease；pending 互斥 ----
    Consumer consumer_state;
    QolSlotDataRequest initial = 0;
    auto spec = consumer_spec(consumer, &consumer_state);
    CHECK(succeeded(slot_data_api().register_section(&spec, &consumer_state.handle, &initial)));
    uint64_t token = 0;
    CHECK(succeeded(save_transition_begin(static_cast<uint32_t>(TransitionKind::kResume), 1, &token)));
    CHECK(token != 0);
    CHECK(save_transition_prepare_stop().code == kQolErrBusy);
    uint64_t second = 0;
    CHECK(save_transition_begin(static_cast<uint32_t>(TransitionKind::kResume), 1, &second).code ==
          kQolErrBusy);
    // lease 已持有：他人 acquire 同槽 Busy。
    QolSlotOperationLease probe{};
    probe.size = sizeof(probe);
    CHECK(qol::infra::slot_operation::acquire(stranger, 1u << 1, &probe).code == kQolErrBusy);
    CHECK(!save_transition_ready_for_save_enter());

    // ---- 观测槽不匹配：Conflict 且 pending 保留 ----
    CHECK(save_transition_confirm(token, 0).code == kQolErrConflict);
    CHECK(save_transition_prepare_stop().code == kQolErrBusy);
    CHECK(consumer_state.ready == 0);

    // ---- confirm 成功（resume）：ensure 幂等、epoch 生成、session_ready、SessionBound load ----
    CHECK(succeeded(save_transition_confirm(token, 1)));
    CHECK(save_transition_ready_for_save_enter());
    CHECK(save_transition_prepare_stop().ok == kQolOk);
    CHECK(consumer_state.ready == 1 && consumer_state.loads == 1);
    QolSlotSession session{-1, 0, 0};
    CHECK(succeeded(slot_data_api().session(&session)));
    CHECK(session.slot == 1 && session.state == static_cast<uint32_t>(QolSlotSessionState::kReady));
    CHECK(session.epoch == 1);  // 旧 epoch 0（无 session）→ 确认序生成新 epoch 1。
    // lease 已释放：他人可 acquire。
    CHECK(succeeded(qol::infra::slot_operation::acquire(stranger, 1u << 1, &probe)));
    CHECK(succeeded(qol::infra::slot_operation::release(stranger, &probe)));

    // ---- 确认失败不消费 pending：ensure 失败保留 token，恢复后同 token 重试成功 ----
    const auto epoch_after_first = session.epoch;
    uint64_t reentry = 0;
    // 同槽重入：old slot=1 + target slot=1 → mask 单槽；新 token + 新 epoch。
    g_ensure_fail = 1;
    CHECK(succeeded(save_transition_begin(static_cast<uint32_t>(TransitionKind::kResume), 1, &reentry)));
    CHECK(reentry > token);
    CHECK(save_transition_confirm(reentry, 1).code == kQolErrInternal);
    CHECK(!save_transition_ready_for_save_enter());  // 失败不发布 ready、不派发后续。
    CHECK(save_transition_prepare_stop().code == kQolErrBusy);
    g_ensure_fail = 0;
    CHECK(succeeded(save_transition_confirm(reentry, 1)));  // 同 token 显式复用重试。
    CHECK(save_transition_ready_for_save_enter());
    CHECK(succeeded(slot_data_api().session(&session)));
    CHECK(session.epoch > epoch_after_first);  // 同槽重入新 epoch。

    // ---- token id 显式复用：cancel 后同槽新 begin 是新 token；旧 id confirm NotFound ----
    uint64_t cancelled = 0;
    CHECK(succeeded(save_transition_begin(static_cast<uint32_t>(TransitionKind::kResume), 1, &cancelled)));
    CHECK(succeeded(save_transition_cancel(cancelled)));
    CHECK(save_transition_prepare_stop().ok == kQolOk);
    CHECK(save_transition_confirm(cancelled, 1).code == kQolErrNotFound);
    uint64_t renewed = 0;
    CHECK(succeeded(save_transition_begin(static_cast<uint32_t>(TransitionKind::kResume), 1, &renewed)));
    CHECK(renewed > cancelled);
    CHECK(save_transition_confirm(renewed, 1).ok == kQolOk);

    // ---- cancel 不触碰磁盘：reset/ensure 计数为 0 ----
    const auto resets_before = g_reset_calls;
    const auto ensures_before = g_ensure_calls;
    uint64_t abandoned = 0;
    CHECK(succeeded(save_transition_begin(static_cast<uint32_t>(TransitionKind::kResume), 2, &abandoned)));
    CHECK(succeeded(save_transition_cancel(abandoned)));
    CHECK(g_reset_calls == resets_before && g_ensure_calls == ensures_before);
    CHECK(succeeded(slot_data_api().session(&session)));
    CHECK(session.state == static_cast<uint32_t>(QolSlotSessionState::kUnavailable));
    CHECK(!save_transition_ready_for_save_enter());

    // ---- begin_new：confirm 走 reset（provider confirm 内部仍做幂等 ensure）----
    uint64_t fresh = 0;
    const auto resets_before_new = g_reset_calls;
    CHECK(succeeded(save_transition_begin(static_cast<uint32_t>(TransitionKind::kBeginNew), 2, &fresh)));
    CHECK(succeeded(save_transition_confirm(fresh, 2)));
    CHECK(g_reset_calls == resets_before_new + 1);

    // ---- journal 严格分派：stage0 恒阻断（不消费 pending）；stage1 需协调器 verdict ----
    SaveJournal prepared{77, 0, 5, JournalStage::kPrepared, {}, {}};
    std::vector<uint8_t> prepared_bytes;
    CHECK(encode_save_journal(prepared, &prepared_bytes));
    g_journal_bytes = prepared_bytes;
    uint64_t with_journal = 0;
    const auto ready_before = consumer_state.ready;
    CHECK(succeeded(save_transition_begin(static_cast<uint32_t>(TransitionKind::kResume), 0, &with_journal)));
    CHECK(save_transition_confirm(with_journal, 0).code == kQolErrBlocked);
    CHECK(consumer_state.ready == ready_before);  // 确认序未推进。
    CHECK(save_transition_prepare_stop().code == kQolErrBusy);
    // stage1 + verdict 注入：放行；无 verdict 阻断。
    SaveJournal confirmed{78, 0, 5, JournalStage::kOriginalConfirmed, {7, {1, 2, 3}}, {}};
    std::vector<uint8_t> confirmed_bytes;
    CHECK(encode_save_journal(confirmed, &confirmed_bytes));
    g_journal_bytes = confirmed_bytes;
    CHECK(save_transition_confirm(with_journal, 0).code == kQolErrBlocked);
    save_transition_set_journal_verdict(&allow_verdict, nullptr);
    CHECK(succeeded(save_transition_confirm(with_journal, 0)));
    CHECK(g_verdict_slot == 0);

    // ---- 协调器健康 gate：拒绝转换开始 ----
    save_transition_set_transition_gate([](int32_t, void*) { return false; }, nullptr);
    uint64_t blocked = 0;
    CHECK(save_transition_begin(static_cast<uint32_t>(TransitionKind::kResume), 1, &blocked).code ==
          kQolErrBusy);
    save_transition_set_transition_gate(nullptr, nullptr);

    // ---- 未注册 token 的 confirm 与未授权 owner ----
    CHECK(save_transition_confirm(999999, 0).code == kQolErrNotFound);
    save_transition_reset_for_test();
    slot_data_set_available(false);
    save_transition_shutdown();
    std::printf("save_transition: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
