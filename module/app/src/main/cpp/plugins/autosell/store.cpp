// autosell_store.cpp —— 自动出售 slot-data participant。

#include "store.h"

#include "config.h"
#include "../../contracts/slot_data.h"
#include "log.h"
#include "plugin.h"
#include "scan.h"

#include <atomic>
#include <array>
#include <cstring>
#include <mutex>
#include <string>

namespace {

// section 名/版本是 sidecar 契约的一部分（跨版本读档稳定性），不得变更。
constexpr const char* kSectionName = "autosell";
constexpr uint16_t kSectionVersion = 2;
constexpr int kSlotCount = 3;

std::mutex g_store_mtx;
std::atomic<qol::contracts::QolSlotDataHandle> g_handle{0};
qol::contracts::QolSlotDataApi g_api{};
qol::contracts::QolSlotDataRequest g_unregister_request = 0;
qol::contracts::QolOwnerToken g_registered_owner = 0;
int g_loaded_slot = -1;
uint64_t g_epoch = 0;
bool g_load_ready = false;
uint64_t g_revision = 0;
bool g_pending_intent = false;
autosell::Config g_pending_config{};
uint64_t g_pending_revision = 0;

struct SlotConfigState {
    bool dirty = false;
    bool has_config = false;
    autosell::Config config{};
    uint64_t revision = 0;
    uint64_t dispatched_revision = 0;
    uint64_t captured_revision = 0;
    bool request_inflight = false;
    qol::contracts::QolSlotDataRequest request = 0;
};
std::array<SlotConfigState, kSlotCount> g_slots{};
uint64_t g_retry_task = 0;
int64_t g_last_retry_frame = -1;
int64_t g_retry_due_frame = -1;
constexpr int64_t kRetryIntervalFrames = 60;

bool same_config(const autosell::Config& a, const autosell::Config& b) {
    return a.enabled == b.enabled && a.rarity == b.rarity && a.enhance == b.enhance &&
           a.socket == b.socket && a.gem_tier == b.gem_tier && a.gem_range == b.gem_range &&
           a.special_mask == b.special_mask;
}

class JsonValidator {
  public:
    explicit JsonValidator(const std::string& input) : input_(input) {}

    bool object_document() {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != '{' || !value(0)) return false;
        whitespace();
        return position_ == input_.size();
    }

  private:
    void whitespace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) ++position_;
    }

    bool string() {
        if (position_ >= input_.size() || input_[position_++] != '"') return false;
        while (position_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[position_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') continue;
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
            if (escaped == 'u') {
                for (int i = 0; i < 4; ++i) {
                    if (position_ >= input_.size()) return false;
                    const char digit = input_[position_++];
                    if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f') ||
                          (digit >= 'A' && digit <= 'F'))) return false;
                }
            } else if (escaped != '"' && escaped != '\\' && escaped != '/' && escaped != 'b' &&
                       escaped != 'f' && escaped != 'n' && escaped != 'r' && escaped != 't') {
                return false;
            }
        }
        return false;
    }

    bool number() {
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9')
                return false;
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') return false;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9')
                ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const size_t fraction = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9')
                ++position_;
            if (fraction == position_) return false;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-'))
                ++position_;
            const size_t exponent = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9')
                ++position_;
            if (exponent == position_) return false;
        }
        return position_ > start;
    }

    bool value(unsigned depth) {
        if (depth > 16) return false;
        whitespace();
        if (position_ >= input_.size()) return false;
        const char c = input_[position_];
        if (c == '"') return string();
        if (c == '{') {
            ++position_;
            whitespace();
            if (position_ < input_.size() && input_[position_] == '}') { ++position_; return true; }
            for (;;) {
                whitespace();
                if (!string()) return false;
                whitespace();
                if (position_ >= input_.size() || input_[position_++] != ':') return false;
                if (!value(depth + 1)) return false;
                whitespace();
                if (position_ >= input_.size()) return false;
                if (input_[position_++] == '}') return true;
                if (input_[position_ - 1] != ',') return false;
            }
        }
        if (c == '[') {
            ++position_;
            whitespace();
            if (position_ < input_.size() && input_[position_] == ']') { ++position_; return true; }
            for (;;) {
                if (!value(depth + 1)) return false;
                whitespace();
                if (position_ >= input_.size()) return false;
                if (input_[position_++] == ']') return true;
                if (input_[position_ - 1] != ',') return false;
            }
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        for (const char* literal : {"true", "false", "null"}) {
            const size_t length = std::strlen(literal);
            if (input_.compare(position_, length, literal) == 0) {
                position_ += length;
                return true;
            }
        }
        return false;
    }

    const std::string& input_;
    size_t position_ = 0;
};

bool query_request(qol::contracts::QolSlotDataRequest request,
                   qol::contracts::QolSlotDataStatus* status) {
    if (request == 0 || g_api.query == nullptr || status == nullptr) return false;
    status->size = sizeof(*status);
    return qol::contracts::succeeded(g_api.query(request, status));
}

bool request_dirty_write(int slot, uint64_t epoch);

int32_t autosell_retry_frame(int64_t frame, void*) {
    autosell_store_flush(frame);
    return 1;
}

bool ensure_retry_task() {
    if (g_retry_task != 0) return true;
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk == nullptr || sdk->scheduler.add == nullptr) return false;
    uint64_t task = 0;
    const QolResult result = sdk->scheduler.add(0, &autosell_retry_frame, nullptr,
                                                static_cast<int32_t>(kRetryIntervalFrames), 0,
                                                &task);
    if (!qol::contracts::succeeded(result) || task == 0) return false;
    g_retry_task = task;
    return true;
}

void remove_retry_task() {
    if (g_retry_task == 0) return;
    const auto* sdk = qol::features::autosell::host_sdk();
    if (sdk != nullptr && sdk->scheduler.remove != nullptr) {
        const QolResult result = sdk->scheduler.remove(g_retry_task);
        if (!qol::contracts::succeeded(result))
            AUTOSELL_LOG_WARN("slot data retry task removal failed id=%llu code=%d",
                              static_cast<unsigned long long>(g_retry_task), result.code);
    }
    g_retry_task = 0;
}

QolResult sd_ok() { return qol::contracts::ok(); }

QolResult sd_error(int code, const char* message) {
    return qol::contracts::error(code, message, static_cast<uint32_t>(std::strlen(message)));
}

QolResult sd_prepare_detach(void*, const qol::contracts::QolSlotSession*, uint32_t) {
    int old_slot = -1;
    uint64_t old_epoch = 0;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        old_slot = g_loaded_slot;
        old_epoch = g_epoch;
        if (g_loaded_slot >= 0 && g_loaded_slot < kSlotCount) {
            auto& state = g_slots[static_cast<size_t>(g_loaded_slot)];
            state.dispatched_revision = 0;
            state.request_inflight = false;
            state.request = 0;
        }
        g_loaded_slot = -1;
        g_epoch = 0;
        g_load_ready = false;
    }
    autosell_scan_session_closed(old_slot, old_epoch);
    return sd_ok();
}

QolResult sd_session_ready(void*, const qol::contracts::QolSlotDataContext* ctx) {
    if (ctx == nullptr || ctx->session.slot < 0 || ctx->session.slot >= kSlotCount)
        return sd_error(kQolErrInvalidArgument, "invalid session context");
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        g_loaded_slot = ctx->session.slot;
        g_epoch = ctx->session.epoch;
        g_load_ready = false;
    }
    autosell_scan_session_closed(-1, 0);
    return sd_ok();
}

QolResult sd_load(void*, const qol::contracts::QolSlotDataContext* ctx, uint32_t presence,
                  uint32_t stored_version, const uint8_t* bytes, uint64_t size) {
    if (ctx == nullptr || ctx->session.slot < 0 || ctx->session.slot >= kSlotCount)
        return sd_error(kQolErrInvalidArgument, "invalid load context");
    const auto kind = static_cast<qol::contracts::QolSlotDataPresence>(presence);
    if (kind == qol::contracts::QolSlotDataPresence::kUnreadable ||
        kind == qol::contracts::QolSlotDataPresence::kVersionUnsupported) {
        AUTOSELL_LOG_WARN("slot data load rejected presence=%u version=%u", presence, stored_version);
        return sd_error(kind == qol::contracts::QolSlotDataPresence::kVersionUnsupported
                            ? kQolErrUnsupported : kQolErrInternal,
                        "autosell slot data unavailable");
    }
    autosell::Config config{};
    if (kind == qol::contracts::QolSlotDataPresence::kPresent) {
        if ((bytes == nullptr && size != 0) || size > 4096) {
            return sd_error(kQolErrInvalidArgument, "invalid autosell payload");
        }
        const std::string json(bytes == nullptr ? "" : reinterpret_cast<const char*>(bytes),
                               static_cast<size_t>(size));
        if (!JsonValidator(json).object_document() ||
            !autosell_config_from_json(json.c_str(), &config)) {
            return sd_error(kQolErrInternal, "invalid autosell payload");
        }
    }
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        if (g_epoch != ctx->session.epoch || g_loaded_slot != ctx->session.slot) {
            return sd_error(kQolErrConflict, "autosell session changed");
        }
        auto& state = g_slots[static_cast<size_t>(ctx->session.slot)];
        if (g_pending_intent) {
            state.config = g_pending_config;
            state.has_config = true;
            state.dirty = true;
            state.revision = g_pending_revision;
            g_pending_intent = false;
        }
        if (state.dirty) {
            config = state.config;
            dirty = true;
        } else {
            state.config = config;
            state.has_config = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        if (g_loaded_slot != ctx->session.slot || g_epoch != ctx->session.epoch)
            return sd_error(kQolErrConflict, "autosell load session changed");
        g_load_ready = true;
    }
    autosell_apply_config(config);
    autosell_set_store_status(ctx->session.slot, ctx->session.slot, false);
    autosell_scan_session_loaded(ctx->session.slot, ctx->session.epoch);
    if (dirty && !request_dirty_write(ctx->session.slot, ctx->session.epoch))
        AUTOSELL_LOG_WARN("slot data catch-up write failed; dirty revision retained slot=%d",
                          ctx->session.slot);
    return sd_ok();
}

QolResult sd_capture(void*, const qol::contracts::QolSlotDataContext* ctx, uint8_t* out,
                     uint64_t capacity, uint64_t* out_size) {
    if (ctx == nullptr || out == nullptr || out_size == nullptr || ctx->session.slot < 0 ||
        ctx->session.slot >= kSlotCount)
        return sd_error(kQolErrInvalidArgument, "invalid capture buffer");
    autosell::Config config;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        auto& state = g_slots[static_cast<size_t>(ctx->session.slot)];
        if (g_loaded_slot != ctx->session.slot || g_epoch != ctx->session.epoch || !state.has_config)
            return sd_error(kQolErrConflict, "autosell capture session mismatch");
        config = state.config;
        state.captured_revision = state.revision;
    }
    const std::string json = autosell_config_to_json(config);
    if (json.size() > capacity) return sd_error(kQolErrInvalidArgument, "autosell payload too large");
    std::memcpy(out, json.data(), json.size());
    *out_size = json.size();
    return sd_ok();
}

void sd_complete(void*, const qol::contracts::QolSlotDataContext* ctx, uint32_t outcome) {
    if (ctx == nullptr) return;
    const bool committed = outcome == static_cast<uint32_t>(qol::contracts::QolSlotDataOutcome::kCommitted);
    bool context_valid = false;
    bool latest_durable = false;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        if (g_epoch == ctx->session.epoch && g_loaded_slot == ctx->session.slot &&
            ctx->session.slot >= 0 && ctx->session.slot < kSlotCount) {
            context_valid = true;
            auto& state = g_slots[static_cast<size_t>(ctx->session.slot)];
            if (committed && state.captured_revision == state.revision) state.dirty = false;
            latest_durable = committed && !state.dirty;
            if (state.dispatched_revision <= state.captured_revision) {
                state.dispatched_revision = 0;
                state.request_inflight = false;
                state.request = 0;
            }
        }
    }
    if (context_valid)
        autosell_set_store_status(ctx->session.slot, ctx->session.slot, latest_durable);
    AUTOSELL_LOG_INFO("slot data write slot=%d committed=%d", ctx->session.slot, committed ? 1 : 0);
    if (committed && context_valid) {
        uint64_t epoch = 0;
        bool dirty = false;
        {
            std::lock_guard<std::mutex> lock(g_store_mtx);
            dirty = ctx->session.slot >= 0 && ctx->session.slot < kSlotCount &&
                    g_slots[static_cast<size_t>(ctx->session.slot)].dirty;
            epoch = g_epoch;
        }
        if (dirty && epoch != 0 && !request_dirty_write(ctx->session.slot, epoch))
            AUTOSELL_LOG_WARN("slot data newer revision remains dirty slot=%d", ctx->session.slot);
    }
}

bool request_dirty_write(int slot, uint64_t epoch) {
    uint64_t revision = 0;
    qol::contracts::QolSlotDataHandle handle = 0;
    qol::contracts::QolSlotDataRequest existing_request = 0;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        if (slot < 0 || slot >= kSlotCount || epoch == 0 || epoch != g_epoch ||
            slot != g_loaded_slot) return false;
        auto& state = g_slots[static_cast<size_t>(slot)];
        if (!state.dirty) return true;
        existing_request = state.request;
        if (state.request_inflight && existing_request == 0) return true;
        revision = state.revision;
        handle = g_handle.load(std::memory_order_acquire);
    }
    if (existing_request != 0) {
        qol::contracts::QolSlotDataStatus status{};
        if (!query_request(existing_request, &status)) {
            AUTOSELL_LOG_WARN("slot data request status unavailable; retain request=%llu",
                              static_cast<unsigned long long>(existing_request));
            return false;
        }
        if (status.state == static_cast<uint32_t>(qol::contracts::QolSlotDataRequestState::kPending))
            return true;
        if (status.state != static_cast<uint32_t>(qol::contracts::QolSlotDataRequestState::kCompleted) &&
            status.state != static_cast<uint32_t>(qol::contracts::QolSlotDataRequestState::kFailed) &&
            status.state != static_cast<uint32_t>(qol::contracts::QolSlotDataRequestState::kCancelled)) {
            AUTOSELL_LOG_WARN("slot data request has unknown state=%u; retain request=%llu",
                              status.state, static_cast<unsigned long long>(existing_request));
            return false;
        }
        std::lock_guard<std::mutex> lock(g_store_mtx);
        auto& state = g_slots[static_cast<size_t>(slot)];
        if (state.request == existing_request) {
            state.request = 0;
            state.dispatched_revision = 0;
            state.request_inflight = false;
        }
    }
    if (handle == 0 || g_api.request_write == nullptr) return false;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        auto& state = g_slots[static_cast<size_t>(slot)];
        if (!state.dirty || epoch != g_epoch || state.revision != revision) return true;
        state.dispatched_revision = revision;
        state.request_inflight = true;
    }
    qol::contracts::QolSlotDataRequest request = 0;
    const QolResult result = g_api.request_write(handle, epoch, &request);
    if (!qol::contracts::succeeded(result)) {
        {
            std::lock_guard<std::mutex> lock(g_store_mtx);
            auto& state = g_slots[static_cast<size_t>(slot)];
            if (state.dispatched_revision == revision) {
                state.dispatched_revision = 0;
                state.request_inflight = false;
            }
        }
        AUTOSELL_LOG_WARN("slot data write request failed slot=%d revision=%llu code=%d",
                          slot, static_cast<unsigned long long>(revision), result.code);
        return false;
    }
    if (request == 0) {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        auto& state = g_slots[static_cast<size_t>(slot)];
        if (state.dispatched_revision == revision) {
            state.dispatched_revision = 0;
            state.request_inflight = false;
        }
        AUTOSELL_LOG_WARN("slot data write returned empty request slot=%d revision=%llu",
                          slot, static_cast<unsigned long long>(revision));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        auto& state = g_slots[static_cast<size_t>(slot)];
        if (state.request_inflight && state.dispatched_revision == revision)
            state.request = request;
    }
    return true;
}

bool store_session_matches(int slot) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    return slot >= 0 && slot < kSlotCount && slot == g_loaded_slot && g_epoch != 0 && g_load_ready;
}

uint64_t store_session_epoch(int slot) {
    std::lock_guard<std::mutex> lock(g_store_mtx);
    return slot >= 0 && slot < kSlotCount && slot == g_loaded_slot && g_epoch != 0 && g_load_ready
               ? g_epoch : 0;
}

void store_flush(int64_t frame) {
    int slot = -1;
    uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        if (g_last_retry_frame >= 0 && frame < g_last_retry_frame) {
            g_retry_due_frame = frame;
        }
        g_last_retry_frame = frame;
        if (g_retry_due_frame >= 0 && frame < g_retry_due_frame) return;
        g_retry_due_frame = frame > INT64_MAX - kRetryIntervalFrames
                                ? INT64_MAX
                                : frame + kRetryIntervalFrames;
        if (!g_load_ready || g_loaded_slot < 0 || g_loaded_slot >= kSlotCount || g_epoch == 0)
            return;
        slot = g_loaded_slot;
        epoch = g_epoch;
        if (!g_slots[static_cast<size_t>(slot)].dirty) return;
    }
    if (!request_dirty_write(slot, epoch))
        AUTOSELL_LOG_WARN("slot data retry deferred slot=%d epoch=%llu",
                          slot, static_cast<unsigned long long>(epoch));
}

}  // namespace

bool autosell_store_session_matches(int slot) { return store_session_matches(slot); }

uint64_t autosell_store_session_epoch(int slot) { return store_session_epoch(slot); }

void autosell_store_reject_session() {
    int slot = -1;
    uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        slot = g_loaded_slot;
        epoch = g_epoch;
        g_load_ready = false;
    }
    autosell_scan_session_closed(slot, epoch);
}

void autosell_store_flush(int64_t frame) { store_flush(frame); }

bool autosell_store_register(qol::contracts::QolOwnerToken owner,
                             const qol::contracts::QolSlotDataApi* api) {
    if (owner == 0 || api == nullptr || api->register_section == nullptr ||
        api->request_write == nullptr || api->unregister == nullptr || api->query == nullptr)
        return false;
    if (g_handle.load(std::memory_order_acquire) != 0) {
        if (g_registered_owner == owner) return true;
        autosell_store_unregister();
        if (g_handle.load(std::memory_order_acquire) != 0) return false;
    }
    g_api = *api;
    if (g_unregister_request != 0) {
        qol::contracts::QolSlotDataStatus status{};
        if (!query_request(g_unregister_request, &status)) {
            AUTOSELL_LOG_WARN("slot data re-register blocked; unregister state unavailable");
            return false;
        }
        if (status.state == static_cast<uint32_t>(qol::contracts::QolSlotDataRequestState::kPending))
            return false;
        g_unregister_request = 0;
        std::lock_guard<std::mutex> lock(g_store_mtx);
        for (auto& state : g_slots) {
            state.dispatched_revision = 0;
            state.request_inflight = false;
            state.request = 0;
        }
    }
    if (!ensure_retry_task()) {
        AUTOSELL_LOG_WARN("slot data retry scheduler unavailable");
        return false;
    }
    const qol::contracts::QolSlotDataSpec spec{
        sizeof(qol::contracts::QolSlotDataSpec), owner, kSectionName, kSectionVersion,
        static_cast<uint32_t>(qol::contracts::QolSlotDataReadPolicy::kSessionBound),
        static_cast<uint32_t>(qol::contracts::QolSlotDataWritePolicy::kImmediate), nullptr,
        &sd_prepare_detach, &sd_session_ready, &sd_load, &sd_capture, &sd_complete};
    qol::contracts::QolSlotDataRequest initial = 0;
    qol::contracts::QolSlotDataHandle handle = 0;
    const auto result = api->register_section(&spec, &handle, &initial);
    if (!qol::contracts::succeeded(result) || handle == 0) {
        remove_retry_task();
        return false;
    }
    g_registered_owner = owner;
    g_handle.store(handle, std::memory_order_release);
    return true;
}

bool autosell_store_persist(int slot, const autosell::Config& config) {
    if (slot < -1 || slot >= kSlotCount) return false;
    uint64_t epoch = 0;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        if (slot == -1) {
            if (!g_pending_intent || !same_config(g_pending_config, config)) {
                if (g_revision == UINT64_MAX) {
                    AUTOSELL_LOG_WARN("slot data revision exhausted; pending intent retained");
                    return false;
                }
                g_pending_config = config;
                g_pending_revision = ++g_revision;
                g_pending_intent = true;
            }
            autosell_set_store_status(slot, g_loaded_slot, false);
            return false;
        }
        if (g_loaded_slot != slot || g_epoch == 0 ||
            g_handle.load(std::memory_order_acquire) == 0) return false;
        auto& state = g_slots[static_cast<size_t>(slot)];
        if (!state.has_config || !same_config(state.config, config)) {
            if (g_revision == UINT64_MAX) {
                AUTOSELL_LOG_WARN("slot data revision exhausted slot=%d", slot);
                return false;
            }
            state.config = config;
            state.has_config = true;
            state.revision = ++g_revision;
            state.dirty = true;
        }
        if (!state.dirty) return true;
        epoch = g_epoch;
    }
    const bool accepted = request_dirty_write(slot, epoch);
    autosell_set_store_status(slot, slot, false);
    return accepted;
}

bool autosell_store_commit_config(const autosell::Config& config) {
    autosell_apply_config(config);
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        if (g_loaded_slot >= 0 && g_epoch != 0) {
            if (!g_load_ready) {
                AUTOSELL_LOG_WARN("config write rejected while slot session is not loaded");
                return false;
            }
            slot = g_loaded_slot;
        }
    }
    const bool accepted = autosell_store_persist(slot, config);
    if (!accepted)
        AUTOSELL_LOG_WARN("config revision retained for retry slot=%d", slot);
    return accepted;
}

void autosell_store_unregister() {
    const auto handle = g_handle.exchange(0, std::memory_order_acq_rel);
    if (handle == 0 || g_api.unregister == nullptr) return;
    remove_retry_task();
    qol::contracts::QolSlotDataRequest request = 0;
    const QolResult result = g_api.unregister(handle, &request);
    if (!qol::contracts::succeeded(result)) {
        AUTOSELL_LOG_WARN("slot data unregister failed code=%d", result.code);
        g_handle.store(handle, std::memory_order_release);
        if (!ensure_retry_task())
            AUTOSELL_LOG_WARN("slot data retry task could not resume after unregister failure");
        return;
    }
    if (request == 0) {
        AUTOSELL_LOG_WARN("slot data unregister returned empty request");
        g_handle.store(handle, std::memory_order_release);
        if (!ensure_retry_task())
            AUTOSELL_LOG_WARN("slot data retry task could not resume after invalid unregister result");
        return;
    }
    g_unregister_request = request;
    g_registered_owner = 0;
    qol::contracts::QolSlotDataStatus status{};
    if (query_request(request, &status) &&
        status.state != static_cast<uint32_t>(qol::contracts::QolSlotDataRequestState::kPending)) {
        g_unregister_request = 0;
        std::lock_guard<std::mutex> lock(g_store_mtx);
        for (auto& state : g_slots) {
            state.dispatched_revision = 0;
            state.request_inflight = false;
            state.request = 0;
        }
    } else {
        AUTOSELL_LOG_INFO("slot data unregister pending request=%llu",
                          static_cast<unsigned long long>(request));
    }
    {
        std::lock_guard<std::mutex> lock(g_store_mtx);
        g_loaded_slot = -1;
        g_epoch = 0;
        g_load_ready = false;
    }
    autosell_scan_session_closed(-1, 0);
}
