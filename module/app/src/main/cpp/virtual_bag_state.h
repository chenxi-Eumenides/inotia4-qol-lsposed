#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#include "stack_codec.h"

namespace virtual_bag {

constexpr int kBagCount = 5;
constexpr int kMaxCapacity = 16;
constexpr int kSlotCount = 16;

// 原版事务域与扩展内部索引都使用 0..4，但两者不是同一个编号空间。原版索引 5
// 是任务袋，任何扩展事务都不得将其作为源、目标、投影窗口或恢复目标。
constexpr int kOriginalTransactionBagCount = kBagCount;
constexpr int kOriginalTaskBag = kOriginalTransactionBagCount;
constexpr int kExtensionLogicalBagFirst = kOriginalTaskBag + 1;
constexpr int kExtensionLogicalBagLast =
    kExtensionLogicalBagFirst + kBagCount - 1;

// ITEMSTATICBASE 静态表镜像（docs/system/bag.md §5，apk/static-data/json/tables/ITEMSTATICBASE.json）：
// 下标 = BagType，容量 4/8/12/16；kNone=0 → 未装备 → 容量 0（ADR-004：固定绘制值不进入最终架构）。
constexpr std::array<uint8_t, 5> kBagTypeCapacities = {0, 4, 8, 12, 16};

// SAVE_SaveItem 记录约束（objdump 确认）：u8 长度前缀 + 18B 头 + 4B×N 词缀，总长 ≤255。
constexpr size_t kSerializedItemBuffer = 256;  // 固定字节数组容量（State 内禁止裸指针）
constexpr size_t kMaxSerializedItem = 255;     // 可序列化最大总长（SAVE_SaveItem 返回值 uxtb 截断）
constexpr size_t kPayloadHeaderSize = 19;      // 1 前缀 + 18 头（真实物品载荷最小长度）
constexpr size_t kPayloadCountOffset = 11;     // 载荷内 u32 数量位域偏移（小端）
constexpr size_t kSerializedItemProbeBuffer = 1024;  // SaveItem 尾部写入校验窗口

enum class PayloadValidation {
    kOk,
    kMissing,
    kLengthOutOfRange,
    kLengthPrefixMismatch,
    kNonZeroTail,
    kUnavailable,
};

inline const char* payload_validation_reason(PayloadValidation validation) {
    switch (validation) {
        case PayloadValidation::kOk: return "ok";
        case PayloadValidation::kMissing: return "payload_missing";
        case PayloadValidation::kLengthOutOfRange: return "payload_length_out_of_range";
        case PayloadValidation::kLengthPrefixMismatch: return "payload_length_prefix_mismatch";
        case PayloadValidation::kNonZeroTail: return "payload_nonzero_tail";
        case PayloadValidation::kUnavailable: return "payload_bridge_unavailable";
    }
    return "payload_validation_unknown";
}

// 所有序列化 payload 的唯一纯校验：长度前缀、范围和 SaveItem 写入后的尾部都必须一致。
inline PayloadValidation validate_serialized_payload_buffer(const uint8_t* payload,
                                                            size_t buffer_size,
                                                            int payload_size) {
    if (payload == nullptr || payload_size == 0) return PayloadValidation::kMissing;
    if (payload_size < static_cast<int>(kPayloadHeaderSize) ||
        payload_size > static_cast<int>(kMaxSerializedItem) ||
        static_cast<size_t>(payload_size) > buffer_size) {
        return PayloadValidation::kLengthOutOfRange;
    }
    if (payload[0] != static_cast<uint8_t>(payload_size - 1)) {
        return PayloadValidation::kLengthPrefixMismatch;
    }
    for (size_t index = static_cast<size_t>(payload_size); index < buffer_size; ++index) {
        if (payload[index] != 0) return PayloadValidation::kNonZeroTail;
    }
    return PayloadValidation::kOk;
}

using SaveItemPayloadFn = int (*)(uint8_t*, void*);
using LoadItemPayloadFn = int (*)(const uint8_t*, void**, int*);
using FreeItemPayloadFn = void (*)(void*);

// SAVE_SaveItem 的纯包装。生产代码和 host stub 共用，确保尾部检查只实现一次。
inline PayloadValidation save_item_payload(SaveItemPayloadFn save_item, void* item,
                                           std::array<uint8_t, kSerializedItemBuffer>* out,
                                           int* out_size) {
    if (save_item == nullptr) return PayloadValidation::kUnavailable;
    if (item == nullptr || out == nullptr || out_size == nullptr) return PayloadValidation::kMissing;

    std::array<uint8_t, kSerializedItemProbeBuffer> probe{};
    const int payload_size = save_item(probe.data(), item);
    const PayloadValidation validation =
        validate_serialized_payload_buffer(probe.data(), probe.size(), payload_size);
    if (validation != PayloadValidation::kOk) return validation;

    out->fill(0);
    std::memcpy(out->data(), probe.data(), static_cast<size_t>(payload_size));
    *out_size = payload_size;
    return PayloadValidation::kOk;
}

enum class ManagedLoadFailure {
    kNone,
    kInvalidPayload,
    kUnavailable,
    kLoadFailed,
    kMissingOutput,
    kConsumedMismatch,
    kReserializeRejected,
    kRoundTripMismatch,
};

inline const char* managed_load_failure_reason(ManagedLoadFailure failure) {
    switch (failure) {
        case ManagedLoadFailure::kNone: return "ok";
        case ManagedLoadFailure::kInvalidPayload: return "invalid_payload";
        case ManagedLoadFailure::kUnavailable: return "payload_bridge_unavailable";
        case ManagedLoadFailure::kLoadFailed: return "load_failed";
        case ManagedLoadFailure::kMissingOutput: return "load_missing_output";
        case ManagedLoadFailure::kConsumedMismatch: return "load_consumed_mismatch";
        case ManagedLoadFailure::kReserializeRejected: return "load_reserialize_rejected";
        case ManagedLoadFailure::kRoundTripMismatch: return "load_round_trip_mismatch";
    }
    return "load_failure_unknown";
}

struct ManagedLoadResult {
    void* item = nullptr;
    PayloadValidation validation = PayloadValidation::kOk;
    ManagedLoadFailure failure = ManagedLoadFailure::kNone;
};

// SAVE_LoadItem 的受管包装。成功时调用方取得临时对象；任意失败都最多归还一次对象。
inline ManagedLoadResult load_item_payload_exact(const uint8_t* payload, int payload_size,
                                                 SaveItemPayloadFn save_item,
                                                 LoadItemPayloadFn load_item,
                                                 FreeItemPayloadFn free_item) {
    ManagedLoadResult result{};
    result.validation =
        validate_serialized_payload_buffer(payload, kSerializedItemBuffer, payload_size);
    if (result.validation != PayloadValidation::kOk) {
        result.failure = ManagedLoadFailure::kInvalidPayload;
        return result;
    }
    if (save_item == nullptr || load_item == nullptr || free_item == nullptr) {
        result.validation = PayloadValidation::kUnavailable;
        result.failure = ManagedLoadFailure::kUnavailable;
        return result;
    }

    void* item = nullptr;
    int consumed = 0;
    const int loaded = load_item(payload, &item, &consumed);
    if (loaded != 1) {
        if (item != nullptr) free_item(item);
        result.failure = ManagedLoadFailure::kLoadFailed;
        return result;
    }
    if (item == nullptr) {
        result.failure = ManagedLoadFailure::kMissingOutput;
        return result;
    }
    if (consumed != payload_size) {
        free_item(item);
        result.failure = ManagedLoadFailure::kConsumedMismatch;
        return result;
    }

    std::array<uint8_t, kSerializedItemBuffer> round_trip{};
    int round_trip_size = 0;
    result.validation = save_item_payload(save_item, item, &round_trip, &round_trip_size);
    if (result.validation != PayloadValidation::kOk) {
        free_item(item);
        result.failure = ManagedLoadFailure::kReserializeRejected;
        return result;
    }
    if (round_trip_size != payload_size ||
        std::memcmp(round_trip.data(), payload, static_cast<size_t>(payload_size)) != 0) {
        free_item(item);
        result.failure = ManagedLoadFailure::kRoundTripMismatch;
        return result;
    }

    result.item = item;
    return result;
}

enum class BagType : uint8_t {
    kNone = 0,
    kHandbag = 1,
    kSmallBackpack = 2,
    kMediumBackpack = 3,
    kLargeBackpack = 4,
};

enum class Mode {
    kOriginal,
    kModule,
    kExitingModule,
};

// P5.2a 纯 session 模型。此层不保存 native 指针，也不解释原版 ABI。
enum class DragPhase : uint8_t {
    kIdle,
    kPressed,
    kNativeMoving,
    kTargetResolved,
    kTransactionInFlight,
    kCancelled,
    kRejected,
    kCommitted,
};

enum class DragTerminalCause : uint8_t {
    kNone,
    kReleased,
    kCancelled,
    kStaleGeneration,
    kInvalidTarget,
    kIllegalEvent,
    kTransactionRejected,
};

enum class DragTransition : uint8_t {
    kAdvanced,
    kIdempotentNoop,
    kIllegal,
};

enum class DragTargetKind : uint8_t {
    kOriginalBag,
    kExtensionSlot,
    kExtensionTab,
    kTaskBagRejected,
    kInvalid,
    kCancel,
};

struct ExtensionDragSession {
    uint32_t protocol_version = 1;
    uint64_t token = 0;
    uint64_t view_generation = 0;
    int source_bag = -1;
    int source_slot = -1;
    DragTargetKind target_kind = DragTargetKind::kInvalid;
    int target_bag = -1;
    int target_slot = -1;
    uint32_t press_sequence = 0;
    uint32_t release_sequence = 0;
    DragPhase phase = DragPhase::kIdle;
    DragTerminalCause terminal_cause = DragTerminalCause::kNone;
    bool transaction_granted = false;
    bool transaction_dispatched = false;
};

inline bool valid_index(int index);
inline bool valid_original_transaction_bag(int bag);
inline bool valid_extension_logical_bag(int bag);

static_assert(!std::is_pointer<decltype(ExtensionDragSession::protocol_version)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::token)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::view_generation)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::source_bag)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::source_slot)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::target_kind)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::target_bag)>::value,
              "drag session must not contain pointers");
static_assert(!std::is_pointer<decltype(ExtensionDragSession::target_slot)>::value,
              "drag session must not contain pointers");

inline bool drag_session_generation_current(const ExtensionDragSession& session,
                                            uint64_t view_generation) {
    return session.view_generation == view_generation;
}

inline DragTransition session_begin(ExtensionDragSession* session, uint64_t token,
                                    uint64_t view_generation, int source_bag, int source_slot) {
    if (session == nullptr || session->phase != DragPhase::kIdle || token == 0 ||
        !valid_index(source_bag) || source_slot < 0 || source_slot >= kSlotCount) {
        return DragTransition::kIllegal;
    }
    session->token = token;
    session->view_generation = view_generation;
    session->source_bag = source_bag;
    session->source_slot = source_slot;
    session->target_kind = DragTargetKind::kInvalid;
    session->target_bag = -1;
    session->target_slot = -1;
    session->press_sequence = 0;
    session->release_sequence = 0;
    session->terminal_cause = DragTerminalCause::kNone;
    session->transaction_granted = false;
    session->transaction_dispatched = false;
    session->phase = DragPhase::kPressed;
    return DragTransition::kAdvanced;
}

inline DragTransition session_on_native_moving(ExtensionDragSession* session,
                                               uint64_t view_generation) {
    if (session == nullptr || !drag_session_generation_current(*session, view_generation)) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase == DragPhase::kNativeMoving) return DragTransition::kIdempotentNoop;
    if (session->phase != DragPhase::kPressed) return DragTransition::kIllegal;
    ++session->press_sequence;
    session->phase = DragPhase::kNativeMoving;
    return DragTransition::kAdvanced;
}

inline DragTransition session_resolve_target(ExtensionDragSession* session,
                                             uint64_t view_generation, DragTargetKind target) {
    if (session == nullptr || !drag_session_generation_current(*session, view_generation) ||
        session->phase == DragPhase::kCancelled || session->phase == DragPhase::kRejected) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase == DragPhase::kTargetResolved && target == DragTargetKind::kExtensionSlot) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase != DragPhase::kNativeMoving || target == DragTargetKind::kCancel) {
        return DragTransition::kIllegal;
    }
    if (target != DragTargetKind::kOriginalBag && target != DragTargetKind::kExtensionSlot &&
        target != DragTargetKind::kExtensionTab) {
        session->phase = DragPhase::kRejected;
        session->terminal_cause = DragTerminalCause::kInvalidTarget;
        return DragTransition::kAdvanced;
    }
    session->phase = DragPhase::kTargetResolved;
    return DragTransition::kAdvanced;
}

inline DragTransition session_begin_transaction(ExtensionDragSession* session,
                                                 uint64_t view_generation) {
    if (session == nullptr || !drag_session_generation_current(*session, view_generation)) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase == DragPhase::kTransactionInFlight) return DragTransition::kIdempotentNoop;
    if (session->phase != DragPhase::kTargetResolved || session->transaction_granted) {
        return DragTransition::kIllegal;
    }
    session->transaction_granted = true;
    session->phase = DragPhase::kTransactionInFlight;
    return DragTransition::kAdvanced;
}

inline DragTransition session_finish_transaction(ExtensionDragSession* session, bool committed) {
    if (session == nullptr || session->phase != DragPhase::kTransactionInFlight) {
        return DragTransition::kIllegal;
    }
    session->phase = committed ? DragPhase::kCommitted : DragPhase::kRejected;
    session->terminal_cause = committed ? DragTerminalCause::kReleased
                                        : DragTerminalCause::kTransactionRejected;
    return DragTransition::kAdvanced;
}

inline DragTransition session_on_release(ExtensionDragSession* session, uint64_t view_generation) {
    if (session == nullptr || !drag_session_generation_current(*session, view_generation)) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase == DragPhase::kCommitted || session->phase == DragPhase::kRejected ||
        session->phase == DragPhase::kCancelled) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase != DragPhase::kTransactionInFlight) return DragTransition::kIllegal;
    ++session->release_sequence;
    return DragTransition::kAdvanced;
}

inline DragTransition session_on_cancel(ExtensionDragSession* session,
                                        uint64_t view_generation) {
    if (session == nullptr || !drag_session_generation_current(*session, view_generation)) {
        return DragTransition::kIdempotentNoop;
    }
    if (session->phase == DragPhase::kCancelled) return DragTransition::kIdempotentNoop;
    if (session->phase != DragPhase::kPressed && session->phase != DragPhase::kNativeMoving &&
        session->phase != DragPhase::kTargetResolved) {
        return DragTransition::kIllegal;
    }
    session->phase = DragPhase::kCancelled;
    session->terminal_cause = DragTerminalCause::kCancelled;
    return DragTransition::kAdvanced;
}

inline bool session_may_create_transaction(const ExtensionDragSession& session) {
    return session.phase == DragPhase::kTransactionInFlight && session.transaction_granted &&
           !session.transaction_dispatched;
}

inline bool session_claim_transaction(ExtensionDragSession* session) {
    if (session == nullptr || !session_may_create_transaction(*session)) return false;
    session->transaction_dispatched = true;
    return true;
}

inline DragTargetKind classify_drag_target(int bag, int slot, bool extension_tab = false,
                                           bool cancelled = false) {
    if (cancelled) return DragTargetKind::kCancel;
    if (extension_tab) return DragTargetKind::kExtensionTab;
    if (bag == kOriginalTaskBag) return DragTargetKind::kTaskBagRejected;
    if (slot < 0 || slot >= kSlotCount) return DragTargetKind::kInvalid;
    if (valid_original_transaction_bag(bag)) return DragTargetKind::kOriginalBag;
    if (valid_extension_logical_bag(bag)) return DragTargetKind::kExtensionSlot;
    return DragTargetKind::kInvalid;
}

struct Item {
    int category = 0;    // 缓存渲染字段（SAVE_LoadItem 头部的物品类别）
    int count = 0;       // 缓存渲染字段（堆叠数量，合并时按 99/999 上限 clamp）
    uint16_t payload_size = 0;  // 0 = 旧版描述符（无可序列化载荷，仅按类别渲染）
    std::array<uint8_t, kSerializedItemBuffer> payload{};  // SAVE_SaveItem 序列化记录（无损数据源）
};

// 跨包移动方向常量。
constexpr uint8_t kTransferOriginalToExtension = 0;
constexpr uint8_t kTransferExtensionToOriginal = 1;
constexpr uint8_t kTransferExtensionToExtension = 2;

// prepare journal 提交阶段（ADR-007）：跨进程恢复时与 committed state、原版世界实态三方对照裁决。
constexpr uint8_t kJournalStagePrepared = 0;        // journal 已落盘，原版保存未执行
constexpr uint8_t kJournalStageOriginalSaved = 1;   // 原版侧变更已持久化，sidecar 未提交
constexpr uint8_t kJournalStageSidecarCommitted = 2; // 两侧均完成，仅待清 journal

// sidecar journal 区段（Kotlin 侧常量镜像，见 ExtensionBagJournal）。
constexpr const char* kJournalSectionName = "extensionbags.journal";
constexpr int kJournalSectionVersion = 1;
constexpr size_t kMaxTransactionIdChars = 64;

// 落盘 journal：与内存 PendingTransfer 携带同样的变更语义，外加事务标识与提交阶段。
struct JournalRecord {
    bool valid = false;
    uint8_t stage = kJournalStagePrepared;
    uint64_t generation = 0;  // 写 journal 时的 sidecar 容器 generation
    char transaction_id[kMaxTransactionIdChars + 1]{};  // NUL 结尾，Kotlin 生成
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

// 原版世界实态探针结果：恢复必须在游戏世界加载后对照真实槽位，不能只信 journal。
struct WorldProbe {
    bool original_slot_holds_payload = false;  // orig→ext：原版源槽仍有源物品；ext→orig：原版目标槽已有该物品
};

// 可恢复事务记录：先写 pending → 提交状态 → 变更原版 → fn_save → 清 pending。
// 载荷语义随方向变化：
//   orig→ext：提交后目标扩展槽应持有的完整载荷（合并时数量位段已修补）；
//   ext→orig：被移动物品的源载荷（恢复时重建入库用）。
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
    // P4.4：事务标识。仅本进程 pending 记录（durable=false），journal_from_pending
    // 原样继承到 JournalRecord v1，保证两侧字段一一映射。
    char transaction_id[kMaxTransactionIdChars + 1]{};
};

// ---- P4.5 失败隔离（内存只读诊断；sidecar 写入时机由 P7 决定，本结构不序列化）----

constexpr size_t kMaxIsolationRecords = 8;

// 隔离 reason 分类（§P4.5 失败分类：空/截断/长度不符归 payload_invalid 的 detail，
// Load 失败、插入失败、域非法为独立 reason）。host 与 native 共用单一来源。
namespace isolation_reason {
constexpr const char* kPayloadInvalid = "payload_invalid";
constexpr const char* kInvalidTransactionDomain = "invalid_transaction_domain";
constexpr const char* kInvalidPayload = "invalid_payload";
constexpr const char* kLoadFailed = "load_failed";
constexpr const char* kInsertFailed = "insert_failed";
constexpr const char* kSlotNotFound = "slot_not_found";
constexpr const char* kJournalInvalid = "journal_invalid";
}  // namespace isolation_reason

struct IsolationRecord {
    bool valid = false;
    uint64_t sequence = 0;        // recordId（进程内递增）
    uint64_t observed_at_ms = 0;  // 调用方注入时间源；0 = host/未知
    const char* reason = "";      // 固定失败分类（静态字符串）
    char transaction_id[kMaxTransactionIdChars + 1]{};
    uint64_t generation = 0;      // journal 场景的 sidecar 容器 generation
    int direction = -1;           // -1 = 不可得；取得的原始值不正则化
    int phase = -1;               // TxnStage/journal stage，-1 = 不可得
    int src_bag = -1;             // 袋/槽原始数值；-1 = 未知；非法值原样保留
    int src_slot = -1;
    int dst_bag = -1;
    int dst_slot = -1;
    uint16_t payload_size = 0;
    std::array<uint8_t, kSerializedItemBuffer> payload{};
    uint16_t source_payload_size = 0;
    std::array<uint8_t, kSerializedItemBuffer> source_payload{};
    const char* detail = "";      // 完整性诊断补充（payload 校验 reason / 失败上下文）
};

struct State {
    std::array<uint8_t, kBagCount> types{};      // 各扩展袋已装备的背包物品 BagType（0=未装备）
    std::array<uint8_t, kBagCount> capacities{}; // 派生容量 = derive_capacity(types[i])，禁止直写
    std::array<std::array<Item, kSlotCount>, kBagCount> items{};
    Mode mode = Mode::kOriginal;
    int original_selected = 0;
    int selected = -1;
    int inspected = -1;   // 槽信息态（扩展物品详情高亮，槽位号）
    int info_bag = -1;    // 袋信息态（二次点击打开，袋号；与槽 inspected 语义分离）
    PendingTransfer pending{};  // 未完成事务（随 JSON 往返持久化，恢复用）
    // P4.5 隔离诊断（内存 only，不进 JSON 序列化，不写 sidecar）。
    std::array<IsolationRecord, kMaxIsolationRecords> isolations{};
    uint8_t isolation_count = 0;   // min(记录数, kMaxIsolationRecords)
    uint8_t isolation_next = 0;    // 环形写入位置
    uint64_t isolation_sequence = 0;
    uint64_t isolation_now_ms = 0; // 调用方在 normalize/隔离前注入的时间源
};

inline void push_isolation(State* state, const IsolationRecord& record) {
    if (state == nullptr || !record.valid) return;
    IsolationRecord copy = record;
    copy.sequence = state->isolation_sequence++;
    copy.valid = true;
    state->isolations[state->isolation_next] = copy;
    state->isolation_next =
        static_cast<uint8_t>((state->isolation_next + 1) % kMaxIsolationRecords);
    if (state->isolation_count < kMaxIsolationRecords) ++state->isolation_count;
}

// 袋解除结果（原版语义：有物品弹窗拒绝，空袋解除）。
enum class UnequipResult {
    kOk,        // 空袋解除：装备清零、容量归 0、标签禁用
    kBlocked,   // 袋内有物品，拒绝解除
    kInvalid,   // 非法袋号
};

enum class ClickResult {
    kIgnored,
    kSelected,
    kInspected,
};

enum class RecoveryAction {
    kRollback,   // 事务未提交：保持现状，仅清除 pending
    kComplete,   // 事务已提交：补做原版侧变更（移除源槽 / 重建入库）
    kNone,       // 无 pending
};

inline bool valid_index(int index) {
    return index >= 0 && index < kBagCount;
}

inline bool valid_original_transaction_bag(int bag) {
    return bag >= 0 && bag < kOriginalTransactionBagCount;
}

inline bool valid_extension_logical_bag(int bag) {
    return bag >= kExtensionLogicalBagFirst && bag <= kExtensionLogicalBagLast;
}

inline int extension_internal_bag(int logical_bag) {
    return valid_extension_logical_bag(logical_bag)
               ? logical_bag - kExtensionLogicalBagFirst
               : -1;
}

inline bool valid_ext2ext_transfer_slots(int src_bag, int src_slot, int dst_bag,
                                         int dst_slot) {
    return valid_index(src_bag) && valid_index(dst_bag) && src_slot >= 0 &&
           src_slot < kSlotCount && dst_slot >= 0 && dst_slot < kSlotCount &&
           (src_bag != dst_bag || src_slot != dst_slot);
}

inline bool valid_transaction_domain(uint8_t direction, int src_bag, int src_slot,
                                      int dst_bag, int dst_slot) {
    if (src_slot < 0 || src_slot >= kSlotCount || dst_slot < 0 || dst_slot >= kSlotCount) {
        return false;
    }
    if (direction == kTransferOriginalToExtension) {
        return valid_original_transaction_bag(src_bag) && valid_index(dst_bag);
    }
    if (direction == kTransferExtensionToOriginal) {
        return valid_index(src_bag) && valid_original_transaction_bag(dst_bag);
    }
    return false;
}

// pending 域校验（进程内事务）：额外接受 ext→ext；journal v1 域校验不复用它，
// 因为 ext→ext 恒不创建 prepare journal（§4.1）。
inline bool valid_pending_transaction_domain(uint8_t direction, int src_bag, int src_slot,
                                             int dst_bag, int dst_slot) {
    if (direction == kTransferExtensionToExtension) {
        return valid_ext2ext_transfer_slots(src_bag, src_slot, dst_bag, dst_slot);
    }
    return valid_transaction_domain(direction, src_bag, src_slot, dst_bag, dst_slot);
}

inline bool valid_pending_transfer_domain(const PendingTransfer& pending) {
    return valid_pending_transaction_domain(pending.direction, pending.src_bag, pending.src_slot,
                                            pending.dst_bag, pending.dst_slot);
}

inline bool valid_pending_transfer_payload(const PendingTransfer& pending) {
    if (validate_serialized_payload_buffer(pending.payload.data(), pending.payload.size(),
                                           pending.payload_size) != PayloadValidation::kOk) {
        return false;
    }
    return pending.source_payload_size == 0 ||
           validate_serialized_payload_buffer(pending.source_payload.data(),
                                              pending.source_payload.size(),
                                              pending.source_payload_size) == PayloadValidation::kOk;
}

inline bool valid_capacity(int capacity) {
    return capacity >= 0 && capacity <= kMaxCapacity;
}

inline bool valid_type(int type) {
    return type >= static_cast<int>(BagType::kNone) &&
           type <= static_cast<int>(BagType::kLargeBackpack);
}

// 容量派生唯一入口：扩展袋容量由其已装备的背包物品 BagType 决定（ITEMSTATICBASE 镜像）。
inline uint8_t derive_capacity(int bag_type) {
    return valid_type(bag_type) ? kBagTypeCapacities[bag_type] : 0;
}

inline bool valid_payload(const Item& item) {
    return validate_serialized_payload_buffer(item.payload.data(), item.payload.size(),
                                              item.payload_size) == PayloadValidation::kOk;
}

inline uint32_t payload_count(const Item& item) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) {
        v |= uint32_t(item.payload[kPayloadCountOffset + i]) << (8 * i);
    }
    return v;
}

inline void patch_payload_count(Item* item, uint32_t count) {
    if (item == nullptr || !valid_payload(*item)) return;
    const uint32_t updated = stack_codec::write_count(payload_count(*item), count);
    for (size_t i = 0; i < 4; ++i) {
        item->payload[kPayloadCountOffset + i] = static_cast<uint8_t>((updated >> (8 * i)) & 0xFF);
    }
}

inline uint32_t payload_hash(const Item& item) {
    uint32_t h = 2166136261u;
    h ^= static_cast<uint32_t>(item.category);
    h *= 16777619u;
    h ^= static_cast<uint32_t>(item.count);
    h *= 16777619u;
    h ^= static_cast<uint32_t>(item.payload_size);
    h *= 16777619u;
    for (size_t i = 0; i < item.payload_size; ++i) {
        h ^= item.payload[i];
        h *= 16777619u;
    }
    return h;
}

// 合并数量：按堆叠上限（stack_limit_enabled ? 999 : 99）clamp。
inline uint32_t merge_count(int existing_count, int added_count, bool extended) {
    const int64_t sum = static_cast<int64_t>(existing_count) + added_count;
    if (sum <= 0) return 0;
    return stack_codec::clamp_count(static_cast<uint32_t>(sum), extended);
}

inline bool mergeable_items(const Item& existing, const Item& source) {
    if (existing.category <= 0 || existing.count <= 0 || source.category <= 0 ||
        source.count <= 0 || existing.category != source.category) {
        return false;
    }
    if (!valid_payload(existing) || !valid_payload(source) ||
        existing.payload_size != source.payload_size) {
        return false;
    }
    Item existing_identity = existing;
    Item source_identity = source;
    patch_payload_count(&existing_identity, 1);
    patch_payload_count(&source_identity, 1);
    return std::memcmp(existing_identity.payload.data(), source_identity.payload.data(),
                       existing_identity.payload_size) == 0;
}

inline bool same_extension_bag_merge_allowed(int src_bag, int dst_bag) {
    return src_bag == dst_bag;
}

inline bool same_extension_bag_mergeable_items(const Item& existing, const Item& source) {
    return existing.category > 0 && existing.count > 0 && source.category > 0 &&
           source.count > 0 && existing.category == source.category && valid_payload(existing) &&
           valid_payload(source);
}

inline bool item_matches_payload(const Item& item, const PendingTransfer& pending) {
    return pending.payload_size > 0 && item.payload_size == pending.payload_size &&
           std::memcmp(item.payload.data(), pending.payload.data(), pending.payload_size) == 0;
}

// 纯判定逻辑（不涉及 native 调用，host 测试覆盖）：
// 以「状态中对应槽的载荷是否等于 pending 载荷」区分事务已提交/未提交，产出确定性恢复动作。
inline RecoveryAction recovery_action(const State& state, const PendingTransfer& pending) {
    if (!pending.valid) return RecoveryAction::kNone;
    if (!valid_pending_transfer_domain(pending) || !valid_pending_transfer_payload(pending)) {
        return RecoveryAction::kRollback;
    }
    if (pending.direction == kTransferOriginalToExtension) {
        return item_matches_payload(state.items[pending.dst_bag][pending.dst_slot], pending)
                   ? RecoveryAction::kComplete
                   : RecoveryAction::kRollback;
    }
    if (pending.direction == kTransferExtensionToExtension) {
        return item_matches_payload(state.items[pending.dst_bag][pending.dst_slot], pending)
                   ? RecoveryAction::kComplete
                   : RecoveryAction::kRollback;
    }
    return item_matches_payload(state.items[pending.src_bag][pending.src_slot], pending)
               ? RecoveryAction::kRollback
               : RecoveryAction::kComplete;
}

// ---- P4.4 进程内五态事务（唯一协调入口的纯模型核心，host 注入测试覆盖）----
// prepared → pending-recorded → logical-state-updated → original-state-updated → committed。
// 状态严格单向推进；失败只允许按阶段回滚（还原逻辑槽与 pending），不做猜测性逆转。
// ext→ext 恒无 original 阶段：不调用原版库存移动，不创建 prepare journal（§4.1）。

enum class TxnStage : uint8_t {
    kPrepared = 0,
    kPendingRecorded,
    kLogicalUpdated,
    kOriginalUpdated,
    kCommitted,
};

// prepared 阶段收集的不可变事务快照；三条移动路径只负责构造它，推进与回滚共享唯一实现。
struct TransactionContext {
    uint8_t direction = kTransferOriginalToExtension;
    uint8_t src_bag = 0;
    uint8_t src_slot = 0;
    uint8_t dst_bag = 0;
    uint8_t dst_slot = 0;
    bool merged = false;
    bool original_stage_applicable = true;
    Item source{};        // 源逻辑项快照（ext→ext/ext→orig 清源还原依据）
    Item committed{};     // 目标提交态（合并时数量位段已修补）
    Item previous_dst{};  // 目标槽先前态（回滚还原依据）
    char transaction_id[kMaxTransactionIdChars + 1]{};
    PendingTransfer pending{};  // txn_build_pending 产出，pending-recorded 阶段写入 State
};

// 依据方向填充 pending（payload 语义与 JournalRecord v1 字段一一映射，durable=false）：
//   orig→ext：payload=目标提交态载荷，source_payload=原版源载荷（恢复时重建原版源）；
//   ext→orig：payload=被移动物品源载荷（恢复时重建入库）；
//   ext→ext：payload=目标提交态载荷。
inline void txn_build_pending(TransactionContext* txn) {
    if (txn == nullptr || txn->transaction_id[0] == '\0') return;
    PendingTransfer pending{};
    pending.valid = true;
    pending.direction = txn->direction;
    pending.src_bag = txn->src_bag;
    pending.src_slot = txn->src_slot;
    pending.dst_bag = txn->dst_bag;
    pending.dst_slot = txn->dst_slot;
    std::strncpy(pending.transaction_id, txn->transaction_id, kMaxTransactionIdChars);
    pending.transaction_id[kMaxTransactionIdChars] = '\0';
    if (txn->direction == kTransferOriginalToExtension) {
        pending.payload_size = txn->committed.payload_size;
        pending.payload = txn->committed.payload;
        pending.source_payload_size = txn->source.payload_size;
        pending.source_payload = txn->source.payload;
    } else if (txn->direction == kTransferExtensionToOriginal) {
        pending.payload_size = txn->source.payload_size;
        pending.payload = txn->source.payload;
    } else {
        pending.payload_size = txn->committed.payload_size;
        pending.payload = txn->committed.payload;
    }
    txn->pending = pending;
}

// logical-state-updated：按方向仅更新扩展逻辑 source/target。
// ext→orig 的扩展源清空发生在原版接管成功之后（§4.3），logical 阶段不改逻辑数组。
inline void txn_apply_logical(State* state, const TransactionContext& txn) {
    if (state == nullptr) return;
    if (txn.direction == kTransferOriginalToExtension) {
        state->items[txn.dst_bag][txn.dst_slot] = txn.committed;
    } else if (txn.direction == kTransferExtensionToExtension) {
        state->items[txn.src_bag][txn.src_slot] = {};
        state->items[txn.dst_bag][txn.dst_slot] = txn.committed;
    }
}

// 按失败阶段还原：先还原逻辑槽，再清 pending。
// 已移交原版库存的对象不参与模块回滚（释放/移交由持有账本的调用层负责）。
inline void txn_rollback_logical(State* state, const TransactionContext& txn, TxnStage failed_at) {
    if (state == nullptr) return;
    if (failed_at >= TxnStage::kCommitted) return;  // 提交后不做猜测性逆转
    if (failed_at > TxnStage::kPendingRecorded) {
        if (txn.direction == kTransferOriginalToExtension) {
            state->items[txn.dst_bag][txn.dst_slot] = txn.previous_dst;
        } else if (txn.direction == kTransferExtensionToExtension) {
            state->items[txn.src_bag][txn.src_slot] = txn.source;
            state->items[txn.dst_bag][txn.dst_slot] = txn.previous_dst;
        }
    }
    state->pending = {};
}

// ---- 跨进程 prepare journal（ADR-007 v1，section: extensionbags.journal）----

enum class JournalRecovery {
    kDiscard,          // journal 损坏/非法：隔离并告警，禁止按其重放（静默覆盖禁令）
    kRollback,         // 原版侧未持久化：清 journal 与 pending，committed 不变
    kReplayToSidecar,  // 原版侧已持久化：把变更应用到 committed 并提交（幂等）
    kJustClear,        // 两侧均完成：仅清 journal
};

// JournalRecord 与 Item 的 payload 一致性（数量位段无关的全量比对，复用 Item 匹配语义）。
inline bool journal_slot_holds_payload(const Item& item, const JournalRecord& journal) {
    PendingTransfer pending{};
    pending.valid = true;
    pending.payload_size = journal.payload_size;
    pending.payload = journal.payload;
    return item_matches_payload(item, pending);
}

inline bool valid_journal_record(const JournalRecord& journal) {
    if (!journal.valid) return false;
    if (journal.stage > kJournalStageSidecarCommitted) return false;
    const size_t id_len = std::strlen(journal.transaction_id);
    if (id_len == 0 || id_len > kMaxTransactionIdChars) return false;
    if (!valid_transaction_domain(journal.direction, journal.src_bag, journal.src_slot,
                                  journal.dst_bag, journal.dst_slot)) {
        return false;
    }
    if (validate_serialized_payload_buffer(journal.payload.data(), journal.payload.size(),
                                           journal.payload_size) != PayloadValidation::kOk) {
        return false;
    }
    return journal.source_payload_size == 0 ||
           validate_serialized_payload_buffer(journal.source_payload.data(),
                                              journal.source_payload.size(),
                                              journal.source_payload_size) == PayloadValidation::kOk;
}

// 三方对照裁决（journal stage × committed state × 原版世界实态）。
// stage 标记可能落后于实态（原版保存成功后、stage 落盘前崩溃），因此世界探针优先于 stage：
//   orig→ext：原版源槽已无源物品 ⇒ 原版侧已持久化 ⇒ 重放扩展侧；仍有 ⇒ 回滚。
//   ext→orig：原版目标槽已有该物品 ⇒ 原版侧已持久化 ⇒ 重放扩展侧（删源槽）；没有 ⇒ 回滚。
// stage=2 或「committed 已等于目标态」时仅需清理（幂等）。
inline JournalRecovery journal_recovery_action(const State& state,
                                               const JournalRecord& journal,
                                               const WorldProbe& probe) {
    if (!valid_journal_record(journal)) return JournalRecovery::kDiscard;
    if (journal.stage == kJournalStageSidecarCommitted) return JournalRecovery::kJustClear;
    const bool original_to_extension = journal.direction == kTransferOriginalToExtension;
    const bool original_side_persisted = original_to_extension
                                             ? !probe.original_slot_holds_payload
                                             : probe.original_slot_holds_payload;
    if (!original_side_persisted) return JournalRecovery::kRollback;
    const bool sidecar_committed =
        original_to_extension
            ? journal_slot_holds_payload(state.items[journal.dst_bag][journal.dst_slot], journal)
            : state.items[journal.src_bag][journal.src_slot].category == 0;
    return sidecar_committed ? JournalRecovery::kJustClear : JournalRecovery::kReplayToSidecar;
}

// P4.5：journal kDiscard（结构/payload 损坏）时保留原始字节为只读隔离记录，
// 供告警与 P7 journal 对照；禁止按其重放。transaction_id 长度不可信，安全截断。
inline IsolationRecord isolation_from_journal_record(const JournalRecord& journal,
                                                     uint64_t observed_at_ms) {
    IsolationRecord record{};
    record.valid = true;
    record.observed_at_ms = observed_at_ms;
    record.reason = isolation_reason::kJournalInvalid;
    std::strncpy(record.transaction_id, journal.transaction_id, kMaxTransactionIdChars);
    record.transaction_id[kMaxTransactionIdChars] = '\0';
    record.generation = journal.generation;
    record.direction = journal.direction;
    record.phase = journal.stage;
    record.src_bag = journal.src_bag;
    record.src_slot = journal.src_slot;
    record.dst_bag = journal.dst_bag;
    record.dst_slot = journal.dst_slot;
    const size_t payload_copy =
        std::min<size_t>(journal.payload_size, journal.payload.size());
    record.payload_size = static_cast<uint16_t>(payload_copy);
    std::memcpy(record.payload.data(), journal.payload.data(), payload_copy);
    const size_t source_copy =
        std::min<size_t>(journal.source_payload_size, journal.source_payload.size());
    record.source_payload_size = static_cast<uint16_t>(source_copy);
    std::memcpy(record.source_payload.data(), journal.source_payload.data(), source_copy);
    record.detail = "kDiscard";
    return record;
}

inline void normalize(State* state) {
    if (state == nullptr) return;
    for (int index = 0; index < kBagCount; ++index) {
        if (!valid_type(state->types[index])) state->types[index] = 0;
        state->capacities[index] = derive_capacity(state->types[index]);
    }
    for (int bag = 0; bag < kBagCount; ++bag) {
        for (int slot = 0; slot < kSlotCount; ++slot) {
            Item& item = state->items[bag][slot];
            if (item.category < 0 || item.count < 0) {
                item = {};
            }
            if (item.payload_size > 0 && (!valid_payload(item) || item.category <= 0)) {
                // 损坏载荷不能降级为 category/count 素体（随机类别会被渲染成完全
                // 不同的物品），也不得默默转空槽：先保留原始字节为只读隔离记录
                // （P4.5），再清槽。
                IsolationRecord record{};
                record.valid = true;
                record.observed_at_ms = state->isolation_now_ms;
                record.reason = isolation_reason::kPayloadInvalid;
                record.src_bag = bag;
                record.src_slot = slot;
                record.payload_size = static_cast<uint16_t>(
                    std::min<size_t>(item.payload_size, item.payload.size()));
                record.payload = item.payload;
                record.detail = item.category <= 0
                                    ? "category_invalid"
                                    : payload_validation_reason(validate_serialized_payload_buffer(
                                          item.payload.data(), item.payload.size(),
                                          item.payload_size));
                push_isolation(state, record);
                item = {};
            }
        }
    }
    if (state->original_selected < 0 || state->original_selected >= 6) {
        state->original_selected = 0;
    }
    if (!valid_index(state->selected)) {
        state->selected = -1;
    }
    if (state->mode == Mode::kModule &&
        (!valid_index(state->selected) || state->capacities[state->selected] == 0)) {
        state->mode = Mode::kOriginal;
        state->selected = -1;
    }
    if (state->info_bag != -1 &&
        (!valid_index(state->info_bag) || state->capacities[state->info_bag] == 0 ||
         state->mode != Mode::kModule)) {
        state->info_bag = -1;
    }
    if (state->inspected != state->selected) state->inspected = -1;
}

inline void enter_original(State* state, int bag) {
    if (state == nullptr) return;
    state->mode = Mode::kOriginal;
    state->original_selected = bag >= 0 && bag < 6 ? bag : 0;
    state->selected = -1;
    state->inspected = -1;
}

inline void begin_exit_module(State* state) {
    if (state == nullptr) return;
    state->mode = Mode::kExitingModule;
    state->inspected = -1;
}

inline bool set_test_equipped(State* state, int index, int bag_type) {
    if (state == nullptr || !valid_index(index) || !valid_type(bag_type) || bag_type == 0) {
        return false;
    }
    state->types[index] = static_cast<uint8_t>(bag_type);
    normalize(state);
    return true;
}

inline bool unequip_bag(State* state, int index) {
    if (state == nullptr || !valid_index(index)) return false;
    for (const Item& item : state->items[index]) {
        if (item.category > 0 || item.count > 0) return false;
    }
    state->types[index] = 0;
    if (state->info_bag == index) state->info_bag = -1;
    if (state->selected == index) {
        state->selected = -1;
        state->mode = Mode::kOriginal;
    }
    normalize(state);
    return true;
}

// 真实装备（P3 装备路径）：占用空闲扩展位；容量经 normalize 由 bag_type
// 派生（与原版 ITEMSTATICBASE 1→4/2→8/3→12/4→16 同源）。已占用位拒绝。
inline bool equip_bag(State* state, int index, int bag_type) {
    if (state == nullptr || !valid_index(index) || !valid_type(bag_type) || bag_type == 0) {
        return false;
    }
    if (state->types[index] != 0) return false;
    state->types[index] = static_cast<uint8_t>(bag_type);
    normalize(state);
    return true;
}

inline ClickResult click(State* state, int index) {
    if (state == nullptr || !valid_index(index)) {
        return ClickResult::kIgnored;
    }
    if (state->capacities[index] == 0) {
        return ClickResult::kIgnored;
    }
    if (state->selected == index) {
        state->mode = Mode::kModule;
        state->info_bag = index;  // 二次点击 = 袋信息态（原版语义：desc_type=1 + MakeDesc）
        state->inspected = -1;
        return ClickResult::kInspected;
    }
    state->mode = Mode::kModule;
    state->selected = index;
    state->inspected = -1;
    state->info_bag = -1;
    return ClickResult::kSelected;
}

inline bool set_item(State* state, int bag, int slot, int category, int count) {
    if (state == nullptr || !valid_index(bag) || slot < 0 || slot >= kSlotCount ||
        category < 0 || count < 0) {
        return false;
    }
    state->items[bag][slot] = Item{category, count};
    return true;
}

// ---- JSON 桥接（Kotlin ExtensionBagUiBridge 往返，payload 以 base64 安全编码）----

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
        const size_t b64_len = static_cast<size_t>(end_quote - b64);
        if (b64_len == 0 || b64_len > 512) return false;
        const int decoded = base64_decode(b64, b64_len, parsed.payload.data(), parsed.payload.size());
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
        const size_t source_len = static_cast<size_t>(source_end - source_b64);
        if (source_len == 0 || source_len > 512) return false;
        const int source_decoded =
            base64_decode(source_b64, source_len, parsed.source_payload.data(),
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

}  // namespace virtual_bag
