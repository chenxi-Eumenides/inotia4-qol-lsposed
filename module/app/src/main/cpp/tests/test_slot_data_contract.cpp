#include "contracts/owner_identity.h"
#include "contracts/slot_data.h"
#include "contracts/slot_data_admin.h"
#include "contracts/plugin.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

using namespace qol::contracts;

#define POD(Type) \
    static_assert(std::is_standard_layout<Type>::value, #Type " layout"); \
    static_assert(std::is_trivially_copyable<Type>::value, #Type " copy"); \
    static_assert(std::is_trivial<Type>::value, #Type " trivial")
#define VALUE(Type, Name, Number) \
    static_assert(static_cast<uint32_t>(Type::Name) == Number, #Type "::" #Name)
#define SIGNATURE(Type, ...) \
    static_assert(std::is_same<Type, __VA_ARGS__>::value, #Type " signature")
#define MEMBER(Type, Name, Fn) \
    static_assert(std::is_same<decltype(Type::Name), Fn>::value, #Type "::" #Name)
#define PREFIX(Type, First) \
    static_assert(offsetof(Type, abi) == 0, #Type " ABI prefix"); \
    MEMBER(Type, abi, QolAbiHeader); \
    static_assert(offsetof(Type, First) == sizeof(QolAbiHeader), #Type " first function")
#define NEXT(Type, Previous, Current) \
    static_assert(offsetof(Type, Current) == offsetof(Type, Previous) + \
                  sizeof(decltype(Type::Previous)), #Type " function order")

POD(QolOwnerIdentityApi);
POD(QolSlotSession);
POD(QolSlotDataContext);
POD(QolSlotDataStatus);
POD(QolSlotDataSpec);
POD(QolSlotDataApi);
POD(QolSlotAdminResult);
POD(QolSlotDataAdminApi);
POD(QolOwnerStoreApi);
SIGNATURE(QolOwnerVerifyTokenFn, QolResult (*)(QolOwnerToken));
static_assert(offsetof(PluginContext, owner_token) >= PLUGIN_CONTEXT_PORTS_STRUCT_SIZE);
static_assert(offsetof(PluginContext, owner_token) + sizeof(QolOwnerToken) == sizeof(PluginContext));
MEMBER(PluginContext, owner_token, QolOwnerToken);
static_assert(offsetof(QolHostSdk, owner_verify_token) == offsetof(QolHostSdk, module_version) +
              sizeof(QolModuleVersionFn));
NEXT(QolHostSdk, owner_verify_token, owner_store);
PREFIX(QolOwnerStoreApi, ensure_slot);
NEXT(QolOwnerStoreApi, ensure_slot, reset_slot);
NEXT(QolOwnerStoreApi, reset_slot, read_section);
NEXT(QolOwnerStoreApi, read_section, write_section);
NEXT(QolOwnerStoreApi, write_section, remove_section);
NEXT(QolOwnerStoreApi, remove_section, replace_sections);
SIGNATURE(decltype(QolOwnerStoreApi::read_section), QolResult (*)(QolOwnerToken, int32_t,
    const char*, uint8_t*, uint64_t, uint64_t*, uint16_t*));
SIGNATURE(decltype(QolOwnerStoreApi::replace_sections), QolResult (*)(QolOwnerToken, int32_t,
    const QolStoreSection*, uint32_t, const char* const*, uint32_t));
SIGNATURE(QolOwnerToken, uint64_t);
SIGNATURE(QolSlotDataHandle, uint64_t);
SIGNATURE(QolSlotDataRequest, uint64_t);
SIGNATURE(QolSlotAdminToken, uint64_t);
SIGNATURE(QolSlotAdminOperation, uint64_t);
SIGNATURE(QolSlotGeneration, uint64_t);
static_assert(kQolOwnerTokenInvalid == 0 && kQolSlotDataHandleInvalid == 0 &&
              kQolSlotDataRequestInvalid == 0 && kQolSlotAdminTokenInvalid == 0 &&
              kQolSlotAdminOperationInvalid == 0);
static_assert(kQolSlotDataVersion.major == 1 && kQolSlotDataVersion.minor == 0);
static_assert(kQolSlotDataAdminVersion.major == 1 && kQolSlotDataAdminVersion.minor == 0);
constexpr bool same_text(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static_assert(same_text(kQolSlotDataCapabilityId, "save.slot_data"));
static_assert(same_text(kQolSlotDataAdminCapabilityId, "save.slot_admin"));
static_assert(kQolSlotDataMaxSectionNameBytes == 64);
static_assert(kQolSlotDataMaxPayloadBytes == 1048576);
static_assert(kQolSlotDataMaxContainerBytes == 4194304);

#define ENUM_WIDTH(Type) \
    SIGNATURE(std::underlying_type_t<Type>, uint32_t); \
    static_assert(sizeof(Type) == 4 && alignof(Type) == alignof(uint32_t))
ENUM_WIDTH(QolSlotDataReadPolicy);
ENUM_WIDTH(QolSlotDataWritePolicy);
ENUM_WIDTH(QolSlotSessionState);
ENUM_WIDTH(QolSlotDataReason);
ENUM_WIDTH(QolSlotDataPresence);
ENUM_WIDTH(QolSlotDataRequestState);
ENUM_WIDTH(QolSlotDataDurability);
ENUM_WIDTH(QolSlotDataOutcome);
ENUM_WIDTH(QolSlotAdminOp);
ENUM_WIDTH(QolSlotAdminApply);
ENUM_WIDTH(QolSlotAdminPhase);
VALUE(QolSlotDataReadPolicy, kImmediate, 0);
VALUE(QolSlotDataReadPolicy, kSessionBound, 1);
VALUE(QolSlotDataWritePolicy, kImmediate, 0);
VALUE(QolSlotDataWritePolicy, kSlotBound, 1);
VALUE(QolSlotSessionState, kUnavailable, 0);
VALUE(QolSlotSessionState, kActivating, 1);
VALUE(QolSlotSessionState, kReady, 2);
VALUE(QolSlotSessionState, kClosing, 3);
VALUE(QolSlotDataReason, kEnter, 0);
VALUE(QolSlotDataReason, kCatchUp, 1);
VALUE(QolSlotDataReason, kRequested, 2);
VALUE(QolSlotDataReason, kSave, 3);
VALUE(QolSlotDataReason, kRecovery, 4);
VALUE(QolSlotDataPresence, kPresent, 0);
VALUE(QolSlotDataPresence, kMissing, 1);
VALUE(QolSlotDataPresence, kUnreadable, 2);
VALUE(QolSlotDataPresence, kVersionUnsupported, 3);
VALUE(QolSlotDataRequestState, kPending, 0);
VALUE(QolSlotDataRequestState, kCompleted, 1);
VALUE(QolSlotDataRequestState, kFailed, 2);
VALUE(QolSlotDataRequestState, kCancelled, 3);
VALUE(QolSlotDataDurability, kStaged, 0);
VALUE(QolSlotDataDurability, kDurable, 1);
VALUE(QolSlotDataDurability, kAbsent, 2);
VALUE(QolSlotDataOutcome, kCommitted, 0);
VALUE(QolSlotDataOutcome, kStaged, 1);
VALUE(QolSlotDataOutcome, kOriginalSaveFailed, 2);
VALUE(QolSlotDataOutcome, kPendingRecovery, 3);
VALUE(QolSlotDataOutcome, kDegraded, 4);
VALUE(QolSlotDataOutcome, kCancelled, 5);
VALUE(QolSlotAdminOp, kExport, 0);
VALUE(QolSlotAdminOp, kImport, 1);
VALUE(QolSlotAdminOp, kDelete, 2);
VALUE(QolSlotAdminOp, kRollback, 3);
VALUE(QolSlotAdminApply, kUnchanged, 0);
VALUE(QolSlotAdminApply, kPrimaryApplied, 1);
VALUE(QolSlotAdminApply, kFullyApplied, 2);
VALUE(QolSlotAdminPhase, kPreDat, 0);
VALUE(QolSlotAdminPhase, kPostDat, 1);

SIGNATURE(QolOwnerQueryFn, QolResult (*)(void*, QolOwnerToken*, uint64_t*));
SIGNATURE(QolOwnerVerifyFn, QolResult (*)(void*, QolOwnerToken));
SIGNATURE(QolSlotDataPrepareDetachFn, QolResult (*)(void*, const QolSlotSession*, uint32_t));
SIGNATURE(QolSlotDataSessionReadyFn, QolResult (*)(void*, const QolSlotDataContext*));
SIGNATURE(QolSlotDataLoadFn, QolResult (*)(void*, const QolSlotDataContext*, uint32_t,
    uint32_t, const uint8_t*, uint64_t));
SIGNATURE(QolSlotDataCaptureFn, QolResult (*)(void*, const QolSlotDataContext*, uint8_t*,
    uint64_t, uint64_t*));
SIGNATURE(QolSlotDataCompleteFn, void (*)(void*, const QolSlotDataContext*, uint32_t));
SIGNATURE(QolSlotDataRegisterFn, QolResult (*)(const QolSlotDataSpec*, QolSlotDataHandle*,
    QolSlotDataRequest*));
SIGNATURE(QolSlotDataRequestLoadFn, QolResult (*)(QolSlotDataHandle, uint64_t, QolSlotDataRequest*));
SIGNATURE(QolSlotDataRequestWriteFn, QolResult (*)(QolSlotDataHandle, uint64_t, QolSlotDataRequest*));
SIGNATURE(QolSlotDataUnregisterFn, QolResult (*)(QolSlotDataHandle, QolSlotDataRequest*));
SIGNATURE(QolSlotDataQueryFn, QolResult (*)(QolSlotDataRequest, QolSlotDataStatus*));
SIGNATURE(QolSlotDataSessionFn, QolResult (*)(QolSlotSession*));
SIGNATURE(QolSlotAdminOpenFn, QolResult (*)(QolOwnerToken, QolSlotAdminToken*));
SIGNATURE(QolSlotAdminBeginFn, QolResult (*)(QolSlotAdminToken, int32_t, uint32_t, uint64_t,
    QolSlotAdminOperation*));
SIGNATURE(QolSlotAdminReadContainerFn, QolResult (*)(QolSlotAdminOperation, uint8_t*, uint64_t, uint64_t*));
SIGNATURE(QolSlotAdminImportContainerFn, QolResult (*)(QolSlotAdminOperation, const uint8_t*, uint64_t));
SIGNATURE(QolSlotAdminDeleteContainerFn, QolResult (*)(QolSlotAdminOperation));
SIGNATURE(QolSlotAdminRestoreContainerFn, QolResult (*)(QolSlotAdminOperation, const uint8_t*, uint64_t));
SIGNATURE(QolSlotAdminEndFn, QolResult (*)(QolSlotAdminOperation, uint32_t, QolSlotAdminResult*));

PREFIX(QolOwnerIdentityApi, query);
NEXT(QolOwnerIdentityApi, query, verify);
MEMBER(QolOwnerIdentityApi, query, QolOwnerQueryFn);
MEMBER(QolOwnerIdentityApi, verify, QolOwnerVerifyFn);
PREFIX(QolSlotDataApi, register_section);
NEXT(QolSlotDataApi, register_section, request_load);
NEXT(QolSlotDataApi, request_load, request_write);
NEXT(QolSlotDataApi, request_write, unregister);
NEXT(QolSlotDataApi, unregister, query);
NEXT(QolSlotDataApi, query, session);
MEMBER(QolSlotDataApi, register_section, QolSlotDataRegisterFn);
MEMBER(QolSlotDataApi, request_load, QolSlotDataRequestLoadFn);
MEMBER(QolSlotDataApi, request_write, QolSlotDataRequestWriteFn);
MEMBER(QolSlotDataApi, unregister, QolSlotDataUnregisterFn);
MEMBER(QolSlotDataApi, query, QolSlotDataQueryFn);
MEMBER(QolSlotDataApi, session, QolSlotDataSessionFn);
PREFIX(QolSlotDataAdminApi, open);
NEXT(QolSlotDataAdminApi, open, begin);
NEXT(QolSlotDataAdminApi, begin, read_container);
NEXT(QolSlotDataAdminApi, read_container, import_container);
NEXT(QolSlotDataAdminApi, import_container, delete_container);
NEXT(QolSlotDataAdminApi, delete_container, restore_container);
NEXT(QolSlotDataAdminApi, restore_container, end);
MEMBER(QolSlotDataAdminApi, open, QolSlotAdminOpenFn);
MEMBER(QolSlotDataAdminApi, begin, QolSlotAdminBeginFn);
MEMBER(QolSlotDataAdminApi, read_container, QolSlotAdminReadContainerFn);
MEMBER(QolSlotDataAdminApi, import_container, QolSlotAdminImportContainerFn);
MEMBER(QolSlotDataAdminApi, delete_container, QolSlotAdminDeleteContainerFn);
MEMBER(QolSlotDataAdminApi, restore_container, QolSlotAdminRestoreContainerFn);
MEMBER(QolSlotDataAdminApi, end, QolSlotAdminEndFn);
MEMBER(QolSlotDataSpec, prepare_detach, QolSlotDataPrepareDetachFn);
MEMBER(QolSlotDataSpec, session_ready, QolSlotDataSessionReadyFn);
MEMBER(QolSlotDataSpec, load, QolSlotDataLoadFn);
MEMBER(QolSlotDataSpec, capture, QolSlotDataCaptureFn);
MEMBER(QolSlotDataSpec, complete, QolSlotDataCompleteFn);
NEXT(QolSlotDataSpec, prepare_detach, session_ready);
NEXT(QolSlotDataSpec, session_ready, load);
NEXT(QolSlotDataSpec, load, capture);
NEXT(QolSlotDataSpec, capture, complete);

// 产品目标与当前 host 为 64 位 ABI；不把这些尺寸当作跨架构打包格式。
static_assert(sizeof(void*) == 8);
static_assert(sizeof(QolAbiHeader) == 8 && alignof(QolAbiHeader) == 4);
static_assert(sizeof(QolSlotSession) == 16 && alignof(QolSlotSession) == 8);
static_assert(offsetof(QolSlotSession, slot) == 0 && offsetof(QolSlotSession, state) == 4 &&
              offsetof(QolSlotSession, epoch) == 8);
static_assert(sizeof(QolSlotDataContext) == 32 && alignof(QolSlotDataContext) == 8);
static_assert(offsetof(QolSlotDataContext, session) == 0 &&
              offsetof(QolSlotDataContext, reason) == 16 &&
              offsetof(QolSlotDataContext, transaction) == 24);
static_assert(sizeof(QolSlotDataStatus) == 64 && alignof(QolSlotDataStatus) == 8);
static_assert(offsetof(QolSlotDataStatus, size) == 0 && offsetof(QolSlotDataStatus, state) == 4 &&
              offsetof(QolSlotDataStatus, durability) == 8 && offsetof(QolSlotDataStatus, reason) == 12 &&
              offsetof(QolSlotDataStatus, presence) == 16 && offsetof(QolSlotDataStatus, stored_version) == 20 &&
              offsetof(QolSlotDataStatus, slot) == 24 && offsetof(QolSlotDataStatus, epoch) == 32 &&
              offsetof(QolSlotDataStatus, result) == 40);
static_assert(sizeof(QolSlotDataSpec) == 88 && alignof(QolSlotDataSpec) == 8);
static_assert(offsetof(QolSlotDataSpec, size) == 0 && offsetof(QolSlotDataSpec, owner) == 8 &&
              offsetof(QolSlotDataSpec, section) == 16 && offsetof(QolSlotDataSpec, section_version) == 24 &&
              offsetof(QolSlotDataSpec, read_policy) == 28 && offsetof(QolSlotDataSpec, write_policy) == 32 &&
              offsetof(QolSlotDataSpec, user) == 40 && offsetof(QolSlotDataSpec, prepare_detach) == 48);
static_assert(sizeof(QolSlotAdminResult) == 64 && alignof(QolSlotAdminResult) == 8);
static_assert(offsetof(QolSlotAdminResult, size) == 0 && offsetof(QolSlotAdminResult, op) == 4 &&
              offsetof(QolSlotAdminResult, apply) == 8 && offsetof(QolSlotAdminResult, reserved) == 12 &&
              offsetof(QolSlotAdminResult, slot) == 16 && offsetof(QolSlotAdminResult, generation_after) == 24 &&
              offsetof(QolSlotAdminResult, bytes) == 32 && offsetof(QolSlotAdminResult, result) == 40);
static_assert(sizeof(QolOwnerIdentityApi) == 24 && alignof(QolOwnerIdentityApi) == 8);
static_assert(sizeof(QolSlotDataApi) == 56 && alignof(QolSlotDataApi) == 8);
static_assert(sizeof(QolSlotDataAdminApi) == 64 && alignof(QolSlotDataAdminApi) == 8);

int main() { return 0; }
