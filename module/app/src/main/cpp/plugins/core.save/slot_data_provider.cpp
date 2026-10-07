#include "slot_data_provider.h"

#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace qol::features::core_save {
namespace {
using namespace contracts;
template <typename T> constexpr uint32_t value(T v) { return static_cast<uint32_t>(v); }
QolResult fail(int32_t code, const char* text) {
    return error(code, text, static_cast<uint32_t>(std::strlen(text)));
}
struct Participant {
    QolSlotDataSpec spec{};
    std::string section;
    uint64_t generation = 0;
    uint32_t references = 0;
    bool closing = false;
    bool dirty = false;
    uint64_t notified_epoch = 0;
    QolSlotDataRequest detach_request = 0;
};
enum class WorkKind { kActivate, kLoad, kWrite };
struct Work {
    WorkKind kind;
    QolSlotDataHandle handle;
    QolSlotDataRequest request;
    uint64_t epoch;
    uint32_t reason;
};
struct Runtime {
    std::mutex mutex;
    QolOwnerToken owner = 0;
    QolOwnerVerifyTokenFn verify = nullptr;
    const QolOwnerStoreApi* backend = nullptr;
    int32_t (*is_main_thread)() = nullptr;
    bool available = false;
    bool busy = false;
    bool transitioning = false;
    int32_t target = -1;
    uint32_t transition_reason = value(QolSlotDataReason::kEnter);
    uint64_t next_handle = 1;
    uint64_t next_request = 1;
    QolSlotSession session{-1, value(QolSlotSessionState::kUnavailable), 0};
    std::map<QolSlotDataHandle, std::shared_ptr<Participant>> participants;
    std::map<QolSlotDataRequest, QolSlotDataStatus> requests;
    std::map<QolSlotDataRequest, std::string> result_messages;
    std::deque<Work> work;
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
QolSlotDataRequest new_request(uint32_t reason) {
    if (g.next_request == std::numeric_limits<uint64_t>::max()) return 0;
    const auto request = g.next_request++;
    QolSlotDataStatus status{};
    status.size = sizeof(status);
    status.state = value(QolSlotDataRequestState::kPending);
    status.durability = value(QolSlotDataDurability::kAbsent);
    status.reason = reason;
    status.presence = value(QolSlotDataPresence::kMissing);
    status.slot = g.session.slot;
    status.epoch = g.session.epoch;
    status.result = ok();
    g.requests.emplace(request, status);
    return request;
}
void settle(QolSlotDataRequest request, QolResult result, bool cancelled = false) {
    auto& status = g.requests.at(request);
    // query 可晚于消费者 user 的释放，结果文本不能继续引用消费者内存。
    auto& message = g.result_messages[request];
    if (result.message != nullptr && result.message_len != 0)
        message.assign(result.message, result.message_len);
    else
        message.clear();
    result.message = message.empty() ? nullptr : message.c_str();
    result.message_len = static_cast<uint32_t>(message.size());
    status.result = result;
    status.state = cancelled ? value(QolSlotDataRequestState::kCancelled)
                            : (succeeded(result) ? value(QolSlotDataRequestState::kCompleted)
                                                 : value(QolSlotDataRequestState::kFailed));
}
void cancel_work(QolSlotDataHandle handle) {
    for (auto it = g.work.begin(); it != g.work.end();) {
        if (it->handle != handle) { ++it; continue; }
        if (it->request != 0) settle(it->request, fail(kQolErrCancelled, "consumer closed"), true);
        it = g.work.erase(it);
    }
}
void finish_detach(QolSlotDataHandle handle, const std::shared_ptr<Participant>& p) {
    if (!p->closing || p->references != 0) return;
    if (p->detach_request != 0) settle(p->detach_request, ok());
    g.participants.erase(handle);
}
bool same_spec(const Participant& p, const QolSlotDataSpec& s) {
    const auto& a = p.spec;
    return !p.closing && a.size == s.size && a.owner == s.owner && p.section == s.section &&
           a.section_version == s.section_version && a.read_policy == s.read_policy &&
           a.write_policy == s.write_policy && a.user == s.user &&
           a.prepare_detach == s.prepare_detach && a.session_ready == s.session_ready &&
           a.load == s.load && a.capture == s.capture && a.complete == s.complete;
}
bool valid_section(const char* section) {
    if (section == nullptr) return false;
    uint32_t length = 0;
    for (; length <= kQolSlotDataMaxSectionNameBytes && section[length] != '\0'; ++length) {
        const char c = section[length];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    }
    // 协调日志属于 provider，消费者不能占用其命名空间。
    return length != 0 && length <= kQolSlotDataMaxSectionNameBytes &&
           std::strncmp(section, "module.save.", 12) != 0 && std::strcmp(section, "module.save") != 0;
}
QolResult register_section(const QolSlotDataSpec* spec, QolSlotDataHandle* handle,
                           QolSlotDataRequest* initial) {
    if (handle != nullptr) *handle = 0;
    if (initial != nullptr) *initial = 0;
    if (spec == nullptr || handle == nullptr || initial == nullptr ||
        spec->size < sizeof(QolSlotDataSpec) || !valid_section(spec->section) ||
        spec->section_version == 0 || spec->read_policy > 1 || spec->write_policy > 1 ||
        spec->prepare_detach == nullptr || spec->session_ready == nullptr || spec->load == nullptr ||
        spec->capture == nullptr || spec->complete == nullptr)
        return fail(kQolErrInvalidArgument, "invalid slot data spec");
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.available) return fail(kQolErrNotReady, "slot data unavailable");
    const auto identity = g.verify(spec->owner);
    if (!succeeded(identity)) return identity;
    for (auto& item : g.participants) {
        if (item.second->section != spec->section) continue;
        if (!same_spec(*item.second, *spec)) return fail(kQolErrConflict, "section registration conflict");
        *handle = item.first;
        return ok();
    }
    if (g.next_handle == std::numeric_limits<uint64_t>::max()) return fail(kQolErrInternal, "handle exhausted");
    auto p = std::make_shared<Participant>();
    p->spec = *spec;
    p->section = spec->section;
    p->spec.section = p->section.c_str();
    p->generation = g.next_handle;
    *handle = g.next_handle++;
    g.participants.emplace(*handle, p);
    if (!g.transitioning && (g.session.state == value(QolSlotSessionState::kReady) ||
                            g.session.state == value(QolSlotSessionState::kActivating))) {
        const auto activation = new_request(value(QolSlotDataReason::kCatchUp));
        if (spec->read_policy == value(QolSlotDataReadPolicy::kSessionBound)) *initial = activation;
        g.work.push_back({WorkKind::kActivate, *handle, activation, g.session.epoch,
                          value(QolSlotDataReason::kCatchUp)});
    }
    return ok();
}
QolResult query(QolSlotDataRequest request, QolSlotDataStatus* status) {
    if (status == nullptr || status->size < sizeof(QolSlotDataStatus))
        return fail(kQolErrInvalidArgument, "invalid status buffer");
    std::lock_guard<std::mutex> lock(g.mutex);
    const auto it = g.requests.find(request);
    if (it == g.requests.end()) return fail(kQolErrNotFound, "request unknown");
    *status = it->second;
    return ok();
}
QolResult session(QolSlotSession* out) {
    if (out == nullptr) return fail(kQolErrInvalidArgument, "invalid session buffer");
    std::lock_guard<std::mutex> lock(g.mutex);
    *out = g.session;
    return ok();
}
QolResult unregister_section(QolSlotDataHandle handle, QolSlotDataRequest* request) {
    if (request == nullptr) return fail(kQolErrInvalidArgument, "invalid unregister output");
    *request = 0;
    std::lock_guard<std::mutex> lock(g.mutex);
    const auto it = g.participants.find(handle);
    if (it == g.participants.end()) return fail(kQolErrNotFound, "handle unknown");
    const auto p = it->second;
    if (p->detach_request == 0) p->detach_request = new_request(value(QolSlotDataReason::kRequested));
    p->closing = true;
    *request = p->detach_request;
    cancel_work(handle);
    finish_detach(handle, p);
    return ok();
}
QolResult request_work(QolSlotDataHandle handle, uint64_t epoch, QolSlotDataRequest* request,
                       WorkKind kind) {
    if (request == nullptr) return fail(kQolErrInvalidArgument, "invalid request output");
    *request = 0;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        const auto it = g.participants.find(handle);
        if (!g.available || g.transitioning || it == g.participants.end() || it->second->closing)
            return fail(kQolErrNotReady, "consumer unavailable");
        if (epoch != g.session.epoch) return fail(kQolErrConflict, "session epoch mismatch");
        if (g.session.state != value(QolSlotSessionState::kReady) &&
            g.session.state != value(QolSlotSessionState::kActivating))
            return fail(kQolErrNotReady, "session not admitted");
        const auto identity = g.verify(it->second->spec.owner);
        if (!succeeded(identity)) return identity;
        *request = new_request(value(QolSlotDataReason::kRequested));
        if (*request == 0) return fail(kQolErrInternal, "request exhausted");
        g.work.push_back({kind, handle, *request, epoch, value(QolSlotDataReason::kRequested)});
    }
    if (main_thread()) slot_data_pump();
    return ok();
}
QolResult request_load(QolSlotDataHandle handle, uint64_t epoch, QolSlotDataRequest* request) {
    return request_work(handle, epoch, request, WorkKind::kLoad);
}
QolResult request_write(QolSlotDataHandle handle, uint64_t epoch, QolSlotDataRequest* request) {
    return request_work(handle, epoch, request, WorkKind::kWrite);
}
const QolSlotDataApi kApi{{sizeof(QolSlotDataApi), 1, 0}, &register_section, &request_load,
                        &request_write, &unregister_section, &query, &session};

bool callback_admitted(const std::shared_ptr<Participant>& p) {
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.available && !p->closing;
}
QolResult load_payload(const std::shared_ptr<Participant>& p, const QolSlotDataContext& ctx,
                       uint32_t* presence, uint32_t* stored_version) {
    std::vector<uint8_t> bytes(kQolSlotDataMaxPayloadBytes);
    uint64_t size = 0;
    uint16_t version = 0;
    auto read = g.backend->read_section(g.owner, ctx.session.slot, p->section.c_str(),
                                       bytes.data(), bytes.size(), &size, &version);
    *stored_version = version;
    *presence = value(QolSlotDataPresence::kPresent);
    if (!succeeded(read)) {
        *presence = read.code == kQolErrNotFound ? value(QolSlotDataPresence::kMissing)
                                               : value(QolSlotDataPresence::kUnreadable);
    } else if (version != p->spec.section_version) {
        *presence = value(QolSlotDataPresence::kVersionUnsupported);
        read = fail(kQolErrUnsupported, "section version unsupported");
    } else if (size > bytes.size()) {
        *presence = value(QolSlotDataPresence::kUnreadable);
        read = fail(kQolErrInvalidArgument, "section payload too large");
    }
    if (!callback_admitted(p)) return fail(kQolErrCancelled, "consumer closed");
    const auto loaded = p->spec.load(p->spec.user, &ctx, *presence, *stored_version,
        *presence == value(QolSlotDataPresence::kPresent) ? bytes.data() : nullptr,
        *presence == value(QolSlotDataPresence::kPresent) ? size : 0);
    if (*presence != value(QolSlotDataPresence::kMissing) && !succeeded(read)) return read;
    return loaded;
}
}  // namespace

const contracts::QolSlotDataApi& slot_data_api() { return kApi; }
QolResult slot_data_initialize(contracts::QolOwnerToken owner, contracts::QolOwnerVerifyTokenFn verify,
                              const contracts::QolOwnerStoreApi* backend, int32_t (*is_main)()) {
    if (owner == 0 || verify == nullptr || backend == nullptr || is_main == nullptr ||
        backend->abi.size < sizeof(*backend) || backend->abi.major != 1 ||
        backend->ensure_slot == nullptr || backend->read_section == nullptr || backend->write_section == nullptr)
        return fail(kQolErrInvalidArgument, "invalid slot data backend");
    const auto identity = verify(owner);
    if (!succeeded(identity)) return identity;
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.busy) return fail(kQolErrNotReady, "slot data callback in flight");
    g.owner = owner;
    g.verify = verify;
    g.backend = backend;
    g.is_main_thread = is_main;
    return ok();
}
void slot_data_set_available(bool available) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.available = available && g.verify != nullptr && g.backend != nullptr;
    if (g.available) return;
    // 停用只撤销准入；不执行 IO，不提前释放回调中的 user 引用。
    for (auto it = g.participants.begin(); it != g.participants.end();) {
        const auto handle = it->first;
        const auto p = it->second;
        ++it;
        p->closing = true;
        cancel_work(handle);
        finish_detach(handle, p);
    }
    g.session.state = value(QolSlotSessionState::kUnavailable);
    g.transitioning = false;
}
void slot_data_pump() {
    if (!main_thread()) return;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (g.busy) return;
        g.busy = true;
    }
    for (;;) {
        Work work{};
        std::shared_ptr<Participant> p;
        QolSlotDataContext ctx{};
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (g.work.empty() || !g.available) { g.busy = false; break; }
            work = g.work.front();
            g.work.pop_front();
            auto it = g.participants.find(work.handle);
            if (it == g.participants.end() || it->second->closing || work.epoch != g.session.epoch ||
                !succeeded(g.verify(it->second->spec.owner))) {
                if (work.request != 0) settle(work.request, fail(kQolErrCancelled, "stale consumer request"), true);
                continue;
            }
            p = it->second;
            ++p->references;
            ctx = {g.session, work.reason, 0};
        }
        QolResult result = ok();
        uint32_t presence = value(QolSlotDataPresence::kMissing);
        uint32_t stored_version = 0;
        uint32_t durability = value(QolSlotDataDurability::kAbsent);
        if (work.kind == WorkKind::kActivate) {
            if (p->notified_epoch != ctx.session.epoch) {
                p->notified_epoch = ctx.session.epoch;
                result = p->spec.session_ready(p->spec.user, &ctx);
            }
            if (succeeded(result) && p->spec.read_policy == value(QolSlotDataReadPolicy::kSessionBound))
                result = load_payload(p, ctx, &presence, &stored_version);
        } else if (work.kind == WorkKind::kLoad) {
            result = load_payload(p, ctx, &presence, &stored_version);
        } else if (p->spec.write_policy == value(QolSlotDataWritePolicy::kSlotBound)) {
            p->dirty = true;
            durability = value(QolSlotDataDurability::kStaged);
            if (callback_admitted(p)) p->spec.complete(p->spec.user, &ctx, value(QolSlotDataOutcome::kStaged));
        } else {
            std::vector<uint8_t> bytes(kQolSlotDataMaxPayloadBytes);
            uint64_t size = 0;
            result = p->spec.capture(p->spec.user, &ctx, bytes.data(), bytes.size(), &size);
            if (succeeded(result) && size > bytes.size()) result = fail(kQolErrInvalidArgument, "capture too large");
            if (succeeded(result) && !callback_admitted(p)) result = fail(kQolErrCancelled, "consumer closed");
            if (succeeded(result)) result = g.backend->write_section(g.owner, ctx.session.slot,
                p->section.c_str(), p->spec.section_version, bytes.data(), size);
            if (succeeded(result)) {
                durability = value(QolSlotDataDurability::kDurable);
                if (callback_admitted(p)) p->spec.complete(p->spec.user, &ctx, value(QolSlotDataOutcome::kCommitted));
            }
        }
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (work.request != 0) {
                auto& status = g.requests.at(work.request);
                status.presence = presence;
                status.stored_version = stored_version;
                status.durability = durability;
                settle(work.request, result, result.code == kQolErrCancelled);
            }
            --p->references;
            finish_detach(work.handle, p);
        }
    }
}
QolResult slot_data_begin_transition(uint32_t reason, int32_t target) {
    if (!main_thread()) return fail(kQolErrBlocked, "transition requires main thread");
    if (target < 0 || target > 2 || reason > value(QolSlotDataReason::kRecovery))
        return fail(kQolErrInvalidArgument, "invalid transition");
    std::vector<std::pair<QolSlotDataHandle, std::shared_ptr<Participant>>> old;
    QolSlotSession session_before{};
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.available || g.busy || g.transitioning) return fail(kQolErrNotReady, "transition busy");
        g.busy = true;
        session_before = g.session;
        g.session.state = value(QolSlotSessionState::kClosing);
        for (auto& item : g.participants) {
            cancel_work(item.first);
            if (!item.second->closing) { ++item.second->references; old.push_back(item); }
        }
    }
    QolResult result = ok();
    for (auto& item : old) {
        if (session_before.state != value(QolSlotSessionState::kUnavailable) && callback_admitted(item.second)) {
            const auto detached = item.second->spec.prepare_detach(item.second->spec.user, &session_before, reason);
            if (!succeeded(detached) && succeeded(result)) result = detached;
        }
        std::lock_guard<std::mutex> lock(g.mutex);
        --item.second->references;
        finish_detach(item.first, item.second);
    }
    std::lock_guard<std::mutex> lock(g.mutex);
    g.busy = false;
    if (!succeeded(result)) return result;  // 拒绝后保留 closing，不能放行原版转换。
    g.target = target;
    g.transition_reason = reason;
    g.transitioning = true;
    g.session.state = value(QolSlotSessionState::kActivating);
    return ok();
}
QolResult slot_data_confirm_transition(int32_t current_slot) {
    if (!main_thread()) return fail(kQolErrBlocked, "confirmation requires main thread");
    std::vector<QolSlotDataRequest> activation_requests;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (!g.available || g.busy || !g.transitioning || current_slot != g.target)
            return fail(kQolErrConflict, "transition target mismatch");
        if (g.session.epoch == std::numeric_limits<uint64_t>::max()) return fail(kQolErrInternal, "epoch exhausted");
        g.busy = true;
    }
    const auto ensured = g.backend->ensure_slot(g.owner, current_slot);
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.busy = false;
        if (!succeeded(ensured)) return ensured;
        g.session.slot = current_slot;
        ++g.session.epoch;
        g.transitioning = false;
        for (auto& item : g.participants) {
            if (item.second->closing) continue;
            const auto request = new_request(g.transition_reason);
            if (request != 0) activation_requests.push_back(request);
            g.work.push_back({WorkKind::kActivate, item.first, request, g.session.epoch, g.transition_reason});
        }
    }
    slot_data_pump();
    std::lock_guard<std::mutex> lock(g.mutex);
    for (const auto request : activation_requests) {
        const auto& status = g.requests.at(request);
        if (status.state != value(QolSlotDataRequestState::kCompleted)) return status.result;
    }
    if (g.available) g.session.state = value(QolSlotSessionState::kReady);
    return ok();
}
}  // namespace qol::features::core_save
