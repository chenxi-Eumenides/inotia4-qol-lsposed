#include "plugins/extension_bag/extension_bag_context.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "contracts/slot_data.h"
#include "plugins/extension_bag/extension_bag_logging.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#if defined(QOL_EXTENSION_BAG_SLOT_DATA_TEST_SEAMS)
#include "tests/extension_bag_slot_data_state_stubs.h"
#else
#include "plugins/extension_bag/runtime_state.h"
#endif

// 实现坐落在全局 namespace（context.h 的声明位置）；契约名经 using 引入。
using namespace qol::contracts;

// ===========================================================================
// P3c：扩展背包接 save.slot_data（SessionBound 读 + SlotBound 写）。
// payload 编解码/校验/默认状态由 EB 内部 codec（virtual_bag state_json /
// parse_state_json_at）承担；容器 IO 归 core.save provider，EB 不再触碰
// ModuleSaveStore / ModuleSaveCoordinator（Kotlin JNI 桥随之退役）。
// ===========================================================================

// game_ui_virtbag.cpp 的共享锁与 dirty 标记（跨 TU 引用；锁边界见 rulebook §2）。
// extern 声明在文件作用域：匿名 namespace 内的声明是内部链接，无法引用其他 TU 的定义。
uint64_t extension_bag_item_revision_locked();

// game_ui_virtbag.cpp（public_runtime.inc）：R-66 保存前校验（容量归一化 + 容量外剔除，
// 持 g_virtual_bag_mtx 调用；EB 唯一的保存前清理点）。
bool virtual_bag_module_save_validate_locked();

namespace {

// 契约名引入：实现坐落在全局 namespace（context.h 的声明位置），
// 匿名 namespace 内统一 qol::contracts 全限定或 using。
using namespace qol::contracts;

constexpr const char* kEbSectionName = "extensionbags.items";
constexpr uint16_t kEbSectionVersion = 4;

// ---- 会话准入状态（provider 回调维护；操作面/排空面读取）----
std::atomic<QolSlotDataHandle> g_slot_data_handle{0};
std::atomic<uint64_t> g_admitted_epoch{0};
// load 不可读/版本不支持、complete degraded、session closing 等都会置位阻断：
// 未加载/epoch 不匹配/降级时扫描与物品操作明确拒绝（不再惰性 IO 兜底）。
std::atomic<bool> g_session_blocked{true};

struct CaptureIdentity {
    uint64_t transaction = 0;
    uint64_t epoch = 0;
    uint64_t revision = 0;
};
CaptureIdentity g_capture{};
uint64_t g_staged_revision = 0;

void eb_reset_loaded_revision_state_locked() {
    g_capture = CaptureIdentity{};
    g_staged_revision = 0;
    g_item_state_dirty = false;
}

QolResult eb_ok() { return qol::contracts::ok(); }

bool occupied_item_nonempty(const virtual_bag::State& state) {
    for (int bag = 0; bag < virtual_bag::kBagCount; ++bag) {
        for (int slot = 0; slot < virtual_bag::kSlotCount; ++slot) {
            const virtual_bag::Item& item = state.items[bag][slot];
            if (item.category > 0 || item.count > 0) return true;
        }
    }
    return false;
}

// prepare_detach（§4）：撤投影、释放缓存、清按钮；拒绝则阻止原版转换。
// 开始即撤销本地准入（g_session_blocked/g_admitted_epoch），detach 窗口内不再放行扫描与物品操作。
QolResult eb_sd_prepare_detach(void* /*user*/, const QolSlotSession* /*old_session*/,
                               uint32_t /*reason*/) {
    g_session_blocked.store(true, std::memory_order_release);
    g_admitted_epoch.store(0, std::memory_order_release);
    return virtual_bag_prepare_save_slot_load() ? eb_ok()
        : qol::contracts::error(kQolErrBlocked, "extension bag detach failed", 24);
}

QolResult eb_sd_session_ready(void* /*user*/, const QolSlotDataContext* ctx) {
    // SessionBound 的自动加载在本通知之后同轮执行；此处只登记已通知 epoch。
    g_admitted_epoch.store(ctx->session.epoch, std::memory_order_release);
    return eb_ok();
}

// bytes 仅本次调用有效；missing 生成默认状态；仅 v4 可解析。非 v4、不可读或解析失败
// 均阻断当前会话，不尝试旧版本迁移，也不以空状态替代损坏数据。
QolResult eb_sd_load(void* /*user*/, const QolSlotDataContext* ctx, uint32_t presence,
                     uint32_t stored_version, const uint8_t* bytes, uint64_t size) {
    virtual_bag::State* state = extension_bag_state();
    if (state == nullptr) return qol::contracts::error(kQolErrInternal, "eb state missing", 15);
    const auto presence_kind = static_cast<qol::contracts::QolSlotDataPresence>(presence);
    if (presence_kind == qol::contracts::QolSlotDataPresence::kMissing) {
        // 默认状态：空背包、原版视图；pending 不存在。
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        *state = {};
        eb_reset_loaded_revision_state_locked();
        g_session_blocked.store(false, std::memory_order_release);
        QOL_LOG_INFO(QolDomain::kExtensionBag, "slot data load slot=%d presence=missing "
                     "epoch=%llu default state", ctx->session.slot,
                     static_cast<unsigned long long>(ctx->session.epoch));
        return eb_ok();
    }
    if (presence_kind == qol::contracts::QolSlotDataPresence::kVersionUnsupported) {
        g_session_blocked.store(true, std::memory_order_release);
        QOL_LOG_WARN(QolDomain::kExtensionBag, "slot data blocked reason=version "
                     "stored=%u expected=%u", stored_version, kEbSectionVersion);
        return qol::contracts::error(kQolErrUnsupported, "section version unsupported", 25);
    }
    if (presence_kind == qol::contracts::QolSlotDataPresence::kUnreadable) {
        g_session_blocked.store(true, std::memory_order_release);
        QOL_LOG_WARN(QolDomain::kExtensionBag, "slot data blocked reason=unreadable");
        return qol::contracts::error(kQolErrInternal, "section unreadable", 20);
    }
    if (size > 0 && bytes == nullptr) {
        g_session_blocked.store(true, std::memory_order_release);
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "slot data blocked reason=missing_payload_bytes slot=%d size=%llu",
                     ctx->session.slot, static_cast<unsigned long long>(size));
        return qol::contracts::error(kQolErrInvalidArgument, "section payload missing", 24);
    }
    std::string payload;
    if (size > 0) payload.assign(reinterpret_cast<const char*>(bytes),
                                 static_cast<std::size_t>(size));
    // v4 payload 中的非法 pending 仅隔离该事务；状态 JSON 解析失败则阻断会话。
    std::lock_guard<std::mutex> state_lock(g_virtual_bag_mtx);
    virtual_bag::PendingTransfer rejected_pending{};
    const bool has_pending = payload.find("\"pending\":") != std::string::npos;
    const auto pending_result =
        has_pending
            ? virtual_bag::parse_pending_transfer_result(payload.c_str(), &rejected_pending)
            : virtual_bag::PendingTransferParseResult::kOk;
    if (pending_result == virtual_bag::PendingTransferParseResult::kInvalidTransactionDomain) {
        QOL_LOG_WARN(QolDomain::kExtensionBag,
                     "pending isolated reason=invalid_transaction_domain direction=%u src=%u/%u dst=%u/%u",
                     static_cast<unsigned int>(rejected_pending.direction),
                     static_cast<unsigned int>(rejected_pending.src_bag),
                     static_cast<unsigned int>(rejected_pending.src_slot),
                     static_cast<unsigned int>(rejected_pending.dst_bag),
                     static_cast<unsigned int>(rejected_pending.dst_slot));
    } else if (pending_result == virtual_bag::PendingTransferParseResult::kMalformed) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "pending isolated reason=malformed_pending_record");
    }
    const bool parsed = virtual_bag::parse_state_json_at(
        payload.c_str(), state, extension_bag_isolation_now_ms(),
        virtual_bag_category_uses_stack_count);
    if (has_pending && pending_result != virtual_bag::PendingTransferParseResult::kOk) {
        virtual_bag::IsolationRecord record{};
        record.valid = true;
        record.observed_at_ms = extension_bag_isolation_now_ms();
        if (pending_result == virtual_bag::PendingTransferParseResult::kInvalidTransactionDomain) {
            record.reason = virtual_bag::isolation_reason::kInvalidTransactionDomain;
            record.direction = rejected_pending.direction;
            record.src_bag = rejected_pending.src_bag;
            record.src_slot = rejected_pending.src_slot;
            record.dst_bag = rejected_pending.dst_bag;
            record.dst_slot = rejected_pending.dst_slot;
            std::strncpy(record.transaction_id, rejected_pending.transaction_id,
                         sizeof(record.transaction_id) - 1);
            record.payload_size = std::min<uint16_t>(
                rejected_pending.payload_size,
                static_cast<uint16_t>(rejected_pending.payload.size()));
            record.payload = rejected_pending.payload;
            record.source_payload_size = std::min<uint16_t>(
                rejected_pending.source_payload_size,
                static_cast<uint16_t>(rejected_pending.source_payload.size()));
            record.source_payload = rejected_pending.source_payload;
            record.detail = "pending_rejected_at_load";
        } else {
            record.reason = virtual_bag::isolation_reason::kInvalidPayload;
            record.detail = "malformed_pending_record";
        }
        virtual_bag::push_isolation(state, record);
    }
    if (!parsed) {
        // 解析失败不写回调用方状态：保持 provider 的 missing 语义？不——字节存在但损坏，
        // 属「不可用」，明确阻断扫描与物品操作（不得拿空状态继续玩）。
        g_session_blocked.store(true, std::memory_order_release);
        QOL_LOG_WARN(QolDomain::kExtensionBag, "slot data blocked reason=parse_failed slot=%d",
                     ctx->session.slot);
        return qol::contracts::error(kQolErrInternal, "state parse failed", 18);
    }
    eb_reset_loaded_revision_state_locked();
    g_session_blocked.store(false, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kExtensionBag, "slot data load slot=%d parsed=1 epoch=%llu",
                 ctx->session.slot, static_cast<unsigned long long>(ctx->session.epoch));
    return eb_ok();
}

// capture：单次调用完成序列化（SlotBound 写入由 provider 在原版保存事务内统一提交）。
// R-66：扩展背包唯一的保存前清理点（容量归一化 + 容量外数据剔除）从旧 participant
// prepare 迁入本回调——保存 prepare 已由协调器承担，校验语义不丢。
QolResult eb_sd_capture(void* /*user*/, const QolSlotDataContext* ctx, uint8_t* out_buffer,
                        uint64_t capacity, uint64_t* out_size) {
    if (out_buffer == nullptr || out_size == nullptr)
        return qol::contracts::error(kQolErrInvalidArgument, "invalid capture buffer", 22);
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    if (!virtual_bag_module_save_validate_locked())
        return qol::contracts::error(kQolErrBlocked, "capacity validation rejected", 24);
    const std::string json = virtual_bag::state_json(*extension_bag_state());
    if (json.size() > capacity)
        return qol::contracts::error(kQolErrInvalidArgument, "capture too large", 16);
    std::memcpy(out_buffer, json.data(), json.size());
    *out_size = json.size();
    g_capture = CaptureIdentity{ctx->transaction, ctx->session.epoch,
                                extension_bag_item_revision_locked()};
    QOL_LOG_INFO(QolDomain::kExtensionBag, "slot data capture slot=%d epoch=%llu bytes=%zu "
                 "nonempty=%d", ctx->session.slot,
                 static_cast<unsigned long long>(ctx->session.epoch), json.size(),
                 occupied_item_nonempty(*extension_bag_state()) ? 1 : 0);
    return eb_ok();
}

// complete：幂等内存记账（不执行 section IO——提交由协调器完成）。
void eb_sd_complete(void* /*user*/, const QolSlotDataContext* ctx, uint32_t outcome) {
    const auto kind = static_cast<qol::contracts::QolSlotDataOutcome>(outcome);
    std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
    switch (kind) {
        case qol::contracts::QolSlotDataOutcome::kCommitted:
            {
                const bool same_capture = g_capture.transaction == ctx->transaction &&
                    g_capture.epoch == ctx->session.epoch &&
                    g_capture.revision == extension_bag_item_revision_locked();
                if (same_capture) {
                    extension_bag_state()->pending = {};
                    g_item_state_dirty = false;
                    g_staged_revision = 0;
                } else {
                    QOL_LOG_WARN(QolDomain::kExtensionBag,
                                 "slot data committed skipped clear reason=capture_identity_changed slot=%d",
                                 ctx->session.slot);
                }
                if (g_capture.transaction == ctx->transaction &&
                    g_capture.epoch == ctx->session.epoch) {
                    g_capture = CaptureIdentity{};
                    g_session_blocked.store(false, std::memory_order_release);
                }
            }
            break;
        case qol::contracts::QolSlotDataOutcome::kStaged:
            // SlotBound 写完成为 staged：等待原版保存事务，不清 dirty。
            break;
        case qol::contracts::QolSlotDataOutcome::kOriginalSaveFailed:
        case qol::contracts::QolSlotDataOutcome::kCancelled:
            // 未提交：保留 dirty/pending，等待下一次保存。
            break;
        case qol::contracts::QolSlotDataOutcome::kPendingRecovery:
        case qol::contracts::QolSlotDataOutcome::kDegraded:
            // pending/degraded：保留证据状态并阻断后续操作，待显式恢复。
            if (g_capture.transaction == ctx->transaction &&
                g_capture.epoch == ctx->session.epoch) {
                g_session_blocked.store(true, std::memory_order_release);
                QOL_LOG_WARN(QolDomain::kExtensionBag,
                             "slot data blocked reason=outcome=%u slot=%d tx=%llu",
                             outcome, ctx->session.slot,
                             static_cast<unsigned long long>(ctx->transaction));
            }
            break;
    }
}

}  // namespace

#if defined(QOL_EXTENSION_BAG_SLOT_DATA_TEST_SEAMS)
void extension_bag_slot_data_complete_for_test(const QolSlotDataContext* ctx, uint32_t outcome) {
    eb_sd_complete(nullptr, ctx, outcome);
}

QolResult extension_bag_slot_data_load_for_test(const QolSlotDataContext* ctx, uint32_t presence,
                                                uint32_t stored_version, const uint8_t* bytes,
                                                uint64_t size) {
    return eb_sd_load(nullptr, ctx, presence, stored_version, bytes, size);
}
#endif

// ---- 注册 / 注销（plugin 生命周期；主线程调用）----

bool extension_bag_slot_data_register(qol::contracts::QolOwnerToken owner,
                                      const QolSlotDataApi* api) {
    if (api == nullptr || api->register_section == nullptr) return false;
    // 重入（owner suspend/resume 换发 token 后的重新注册）：先注销旧注册，避免旧凭据
    // 参与者与新 owner token 在同一 section 上冲突（same_spec 要求 owner 相同）。
    if (g_slot_data_handle.load(std::memory_order_acquire) != 0) {
        (void)extension_bag_slot_data_unregister(api);
    }
    // 注册只复制规格：spec 在栈上按本次 install 注入的 owner 构造（suspend/resume 会
    // 换发 token，不得缓存到静态存储）。
    const QolSlotDataSpec spec{
        sizeof(QolSlotDataSpec),
        owner,
        kEbSectionName,
        kEbSectionVersion,
        static_cast<uint32_t>(qol::contracts::QolSlotDataReadPolicy::kSessionBound),
        static_cast<uint32_t>(qol::contracts::QolSlotDataWritePolicy::kSlotBound),
        nullptr,
        &eb_sd_prepare_detach,
        &eb_sd_session_ready,
        &eb_sd_load,
        &eb_sd_capture,
        &eb_sd_complete,
    };
    QolSlotDataHandle handle = 0;
    QolSlotDataRequest initial = 0;
    const auto registered = api->register_section(&spec, &handle, &initial);
    if (!qol::contracts::succeeded(registered) || handle == 0) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "slot data register failed code=%d",
                     registered.code);
        return false;
    }
    g_slot_data_handle.store(handle, std::memory_order_release);
    QOL_LOG_INFO(QolDomain::kExtensionBag, "slot data registered handle=%llu initial_req=%llu",
                 static_cast<unsigned long long>(handle),
                 static_cast<unsigned long long>(initial));
    return true;
}

QolSlotDataRequest extension_bag_slot_data_unregister(const QolSlotDataApi* api) {
    const QolSlotDataHandle handle = g_slot_data_handle.exchange(0, std::memory_order_acq_rel);
    if (handle == 0 || api == nullptr || api->unregister == nullptr) return 0;
    // 先关准入：注销在途期间扫描与物品操作拒绝（终态后零回调由 provider 保证）。
    g_session_blocked.store(true, std::memory_order_release);
    g_admitted_epoch.store(0, std::memory_order_release);
    QolSlotDataRequest request = 0;
    const auto result = api->unregister(handle, &request);
    if (!qol::contracts::succeeded(result)) {
        QOL_LOG_WARN(QolDomain::kExtensionBag, "slot data unregister failed code=%d", result.code);
        return 0;
    }
    return request;
}

bool extension_bag_slot_data_request_settled(const QolSlotDataApi* api,
                                             QolSlotDataRequest request) {
    if (api == nullptr || api->query == nullptr || request == 0) return true;
    qol::contracts::QolSlotDataStatus status{};
    status.size = sizeof(status);
    if (!qol::contracts::succeeded(api->query(request, &status))) return true;  // 未知即终态。
    return status.state != static_cast<uint32_t>(qol::contracts::QolSlotDataRequestState::kPending);
}

// ---- 会话准入（锁内查询；扫描与物品操作在准入失败时明确拒绝）----

bool extension_bag_session_admitted_locked() {
    return !g_session_blocked.load(std::memory_order_acquire) &&
           g_slot_data_handle.load(std::memory_order_acquire) != 0 &&
           g_admitted_epoch.load(std::memory_order_acquire) != 0;
}

// ---- dirty staging 与安全点排空（P3c：43 处 persist 死桩的替代路径）----
// 每个 dirty revision 至多通知一次 SlotBound request_write；capture 仍序列化全量状态。
void extension_bag_flush_dirty_staging(const QolSlotDataApi* api) {
    if (api == nullptr || api->request_write == nullptr) return;
    uint64_t epoch = 0;
    uint64_t revision = 0;
    {
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (!g_item_state_dirty || !extension_bag_session_admitted_locked()) return;
        epoch = g_admitted_epoch.load(std::memory_order_acquire);
        revision = extension_bag_item_revision_locked();
        if (revision == g_staged_revision) return;
        g_staged_revision = revision;
    }
    QolSlotDataRequest request = 0;
    const auto result = api->request_write(g_slot_data_handle.load(std::memory_order_acquire),
                                           epoch, &request);
    if (!qol::contracts::succeeded(result)) {
        // 非主线程投递失败/epoch 不匹配：保留 dirty，下一安全点重试。
        std::lock_guard<std::mutex> lock(g_virtual_bag_mtx);
        if (g_staged_revision == revision) g_staged_revision = 0;
        return;
    }
}
