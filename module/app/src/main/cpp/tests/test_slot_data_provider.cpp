#include "contracts/slot_data.h"
#include "host/owner_identity.h"
#include "plugins/core.save/slot_data_provider.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace qol::contracts;
using namespace qol::features::core_save;
static int g_pass = 0;
static int g_fail = 0;
#define CHECK(condition) do { if (condition) ++g_pass; else { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

namespace {
const auto g_main = std::this_thread::get_id();
int32_t is_main() { return std::this_thread::get_id() == g_main; }
struct Stored { uint16_t version; std::vector<uint8_t> bytes; };
std::map<std::pair<int32_t, std::string>, Stored> g_disk;
int g_read_error = 0;
int g_writes = 0;
QolResult ensure(QolOwnerToken token, int32_t) {
    return qol::host::owner_authorize_store(token, "module-saves");
}
QolResult read(QolOwnerToken token, int32_t slot, const char* section, uint8_t* out,
               uint64_t capacity, uint64_t* size, uint16_t* version) {
    CHECK(is_main());
    const auto authorized = ensure(token, slot);
    if (!succeeded(authorized)) return authorized;
    if (g_read_error) return error(g_read_error, "unreadable", 10);
    auto it = g_disk.find({slot, section});
    if (it == g_disk.end()) return error(kQolErrNotFound, "missing", 7);
    *version = it->second.version;
    *size = it->second.bytes.size();
    if (*size > capacity) return error(kQolErrInvalidArgument, "too large", 9);
    if (*size != 0) std::memcpy(out, it->second.bytes.data(), *size);
    return ok();
}
QolResult write(QolOwnerToken token, int32_t slot, const char* section, uint16_t version,
                const uint8_t* bytes, uint64_t size) {
    CHECK(is_main());
    const auto authorized = ensure(token, slot);
    if (!succeeded(authorized)) return authorized;
    ++g_writes;
    g_disk[{slot, section}] = {version, std::vector<uint8_t>(bytes, bytes + size)};
    return ok();
}
const QolOwnerStoreApi kBackend{{sizeof(QolOwnerStoreApi), 1, 0}, &ensure, nullptr,
                               &read, &write, nullptr, nullptr};
struct Consumer {
    int ready = 0;
    int loads = 0;
    int captures = 0;
    int completes = 0;
    int detaches = 0;
    uint32_t presence = 0;
    uint32_t version = 0;
    bool detach_in_capture = false;
    bool reject_detach = false;
    QolSlotDataHandle handle = 0;
    QolSlotDataRequest unregister_request = 0;
};
QolSlotDataStatus status(QolSlotDataRequest request) {
    QolSlotDataStatus out{};
    out.size = sizeof(out);
    CHECK(succeeded(slot_data_api().query(request, &out)));
    return out;
}
QolSlotSession session() {
    QolSlotSession out{};
    CHECK(succeeded(slot_data_api().session(&out)));
    return out;
}
QolResult detach(void* user, const QolSlotSession*, uint32_t) {
    CHECK(is_main());
    auto& c = *static_cast<Consumer*>(user);
    ++c.detaches;
    return c.reject_detach ? error(kQolErrBlocked, "detach rejected", 15) : ok();
}
QolResult ready(void* user, const QolSlotDataContext*) {
    CHECK(is_main());
    ++static_cast<Consumer*>(user)->ready;
    return ok();
}
QolResult load(void* user, const QolSlotDataContext*, uint32_t presence, uint32_t version,
               const uint8_t* bytes, uint64_t size) {
    CHECK(is_main());
    auto& c = *static_cast<Consumer*>(user);
    ++c.loads;
    c.presence = presence;
    c.version = version;
    if (presence != static_cast<uint32_t>(QolSlotDataPresence::kPresent)) CHECK(bytes == nullptr && size == 0);
    return ok();
}
QolResult capture(void* user, const QolSlotDataContext*, uint8_t* out, uint64_t capacity,
                  uint64_t* size) {
    CHECK(is_main());
    auto& c = *static_cast<Consumer*>(user);
    ++c.captures;
    CHECK(capacity >= 1);
    out[0] = 42;
    *size = 1;
    if (c.detach_in_capture) {
        CHECK(succeeded(slot_data_api().unregister(c.handle, &c.unregister_request)));
        CHECK(status(c.unregister_request).state == static_cast<uint32_t>(QolSlotDataRequestState::kPending));
        QolSlotDataRequest refused = 0;
        CHECK(!succeeded(slot_data_api().request_write(c.handle, session().epoch, &refused)));
    }
    return ok();
}
void complete(void* user, const QolSlotDataContext*, uint32_t) {
    CHECK(is_main());
    ++static_cast<Consumer*>(user)->completes;
}
QolSlotDataSpec spec(QolOwnerToken token, const char* name, Consumer* c,
                     QolSlotDataReadPolicy rp = QolSlotDataReadPolicy::kSessionBound,
                     QolSlotDataWritePolicy wp = QolSlotDataWritePolicy::kSlotBound) {
    return {sizeof(QolSlotDataSpec), token, name, 1, static_cast<uint32_t>(rp),
            static_cast<uint32_t>(wp), c, &detach, &ready, &load, &capture, &complete};
}
}  // namespace

int main() {
    int provider_identity = 0, consumer_identity = 0, other_identity = 0;
    CHECK(qol::host::owner_register(&provider_identity));
    CHECK(qol::host::owner_register(&consumer_identity));
    CHECK(qol::host::owner_register(&other_identity));
    CHECK(qol::host::owner_bind_store(&provider_identity, "module-saves"));
    auto& identity = qol::host::owner_identity_api();
    QolOwnerToken provider = 0, consumer = 0, other = 0;
    uint64_t generation = 0;
    CHECK(succeeded(identity.query(&provider_identity, &provider, &generation)));
    CHECK(succeeded(identity.query(&consumer_identity, &consumer, &generation)));
    const auto original_generation = generation;
    CHECK(succeeded(identity.query(&other_identity, &other, &generation)));
    CHECK(provider != consumer && consumer != other);
    CHECK(!succeeded(qol::host::owner_authorize_store(consumer, "module-saves")));
    CHECK(!succeeded(identity.verify(&other_identity, consumer)));
    CHECK(!succeeded(qol::host::owner_verify_token(0)));
    CHECK(!succeeded(qol::host::owner_verify_token(UINT64_MAX)));
    CHECK(qol::host::owner_register(&consumer_identity));
    QolOwnerToken duplicate = 0;
    CHECK(succeeded(identity.query(&consumer_identity, &duplicate, &generation)));
    CHECK(duplicate == consumer && generation == original_generation);
    CHECK(succeeded(slot_data_initialize(provider, &qol::host::owner_verify_token, &kBackend, &is_main)));
    slot_data_set_available(true);
    CHECK(succeeded(slot_data_begin_transition(0, 0)));
    CHECK(session().state == static_cast<uint32_t>(QolSlotSessionState::kActivating));
    CHECK(succeeded(slot_data_confirm_transition(0)));
    const auto epoch = session().epoch;
    Consumer bound;
    QolSlotDataRequest initial = 0;
    auto bound_spec = spec(0, "test.bound", &bound);
    CHECK(!succeeded(slot_data_api().register_section(&bound_spec, &bound.handle, &initial)));
    bound_spec.owner = consumer;
    CHECK(succeeded(slot_data_api().register_section(&bound_spec, &bound.handle, &initial)));
    CHECK(initial != 0 && bound.loads == 0 && bound.ready == 0);
    QolSlotDataHandle repeated = 0;
    QolSlotDataRequest repeated_request = 0;
    CHECK(succeeded(slot_data_api().register_section(&bound_spec, &repeated, &repeated_request)));
    CHECK(repeated == bound.handle && repeated_request == 0);
    Consumer conflicting;
    auto conflict = spec(other, "test.bound", &conflicting);
    CHECK(!succeeded(slot_data_api().register_section(&conflict, &repeated, &repeated_request)));
    slot_data_pump();
    CHECK(bound.loads == 1 && bound.ready == 1);
    CHECK(status(initial).state == static_cast<uint32_t>(QolSlotDataRequestState::kCompleted));
    CHECK(bound.presence == static_cast<uint32_t>(QolSlotDataPresence::kMissing));
    QolSlotDataRequest request = 0;
    CHECK(!succeeded(slot_data_api().request_load(bound.handle, epoch + 1, &request)));
    CHECK(!succeeded(slot_data_api().request_write(bound.handle, epoch + 1, &request)));
    CHECK(succeeded(slot_data_api().request_write(bound.handle, epoch, &request)));
    CHECK(status(request).durability == static_cast<uint32_t>(QolSlotDataDurability::kStaged));
    CHECK(bound.captures == 0 && g_writes == 0);

    Consumer immediate;
    auto immediate_spec = spec(consumer, "test.immediate", &immediate,
                              QolSlotDataReadPolicy::kImmediate, QolSlotDataWritePolicy::kImmediate);
    CHECK(succeeded(slot_data_api().register_section(&immediate_spec, &immediate.handle, &initial)));
    CHECK(initial == 0 && immediate.ready == 0);
    slot_data_pump();
    CHECK(immediate.ready == 1 && immediate.loads == 0);
    CHECK(succeeded(slot_data_api().request_load(immediate.handle, epoch, &request)));
    CHECK(immediate.loads == 1 && immediate.presence == static_cast<uint32_t>(QolSlotDataPresence::kMissing));
    g_read_error = kQolErrInternal;
    CHECK(succeeded(slot_data_api().request_load(immediate.handle, epoch, &request)));
    CHECK(immediate.presence == static_cast<uint32_t>(QolSlotDataPresence::kUnreadable));
    CHECK(status(request).state == static_cast<uint32_t>(QolSlotDataRequestState::kFailed));
    g_read_error = 0;
    g_disk[{0, "test.immediate"}] = {2, {9}};
    CHECK(succeeded(slot_data_api().request_load(immediate.handle, epoch, &request)));
    CHECK(immediate.presence == static_cast<uint32_t>(QolSlotDataPresence::kVersionUnsupported));
    CHECK(immediate.version == 2 && status(request).stored_version == 2);
    CHECK(succeeded(slot_data_api().request_write(immediate.handle, epoch, &request)));
    CHECK(status(request).durability == static_cast<uint32_t>(QolSlotDataDurability::kDurable));
    CHECK(immediate.captures == 1 && g_writes == 1);
    CHECK(succeeded(slot_data_api().request_load(immediate.handle, epoch, &request)));
    CHECK(immediate.presence == static_cast<uint32_t>(QolSlotDataPresence::kPresent) && immediate.version == 1);
    std::thread background([&] {
        CHECK(succeeded(slot_data_api().request_write(immediate.handle, epoch, &request)));
        CHECK(status(request).state == static_cast<uint32_t>(QolSlotDataRequestState::kPending));
    });
    background.join();
    CHECK(immediate.captures == 1);
    slot_data_pump();
    CHECK(immediate.captures == 2 && status(request).state == static_cast<uint32_t>(QolSlotDataRequestState::kCompleted));

    immediate.detach_in_capture = true;
    CHECK(succeeded(slot_data_api().request_write(immediate.handle, epoch, &request)));
    CHECK(status(immediate.unregister_request).state == static_cast<uint32_t>(QolSlotDataRequestState::kCompleted));
    const auto callback_count = immediate.loads + immediate.captures + immediate.completes + immediate.ready;
    CHECK(!succeeded(slot_data_api().request_load(immediate.handle, epoch, &request)));
    slot_data_pump();
    CHECK(callback_count == immediate.loads + immediate.captures + immediate.completes + immediate.ready);
    CHECK(g_disk.count({0, "test.immediate"}) == 1);
    const auto old_handle = immediate.handle;
    immediate.detach_in_capture = false;
    CHECK(succeeded(slot_data_api().register_section(&immediate_spec, &immediate.handle, &initial)));
    CHECK(immediate.handle > old_handle);
    slot_data_pump();
    CHECK(succeeded(slot_data_begin_transition(0, 0)));
    CHECK(!succeeded(slot_data_api().request_load(bound.handle, epoch, &request)));
    CHECK(succeeded(slot_data_confirm_transition(0)));
    CHECK(session().epoch > epoch && bound.loads == 2);
    CHECK(!succeeded(slot_data_api().request_write(bound.handle, epoch, &request)));
    const auto same_slot_epoch = session().epoch;
    CHECK(succeeded(slot_data_begin_transition(0, 1)));
    CHECK(!succeeded(slot_data_confirm_transition(0)));
    CHECK(succeeded(slot_data_confirm_transition(1)) && session().epoch > same_slot_epoch);

    qol::host::owner_suspend(&consumer_identity);
    CHECK(!succeeded(qol::host::owner_verify_token(consumer)));
    QolOwnerToken stopped = 99;
    CHECK(succeeded(identity.query(&consumer_identity, &stopped, &generation)));
    CHECK(stopped == 0 && generation == original_generation + 1);
    CHECK(qol::host::owner_resume(&consumer_identity));
    CHECK(succeeded(identity.query(&consumer_identity, &duplicate, &generation)));
    CHECK(duplicate > consumer && succeeded(qol::host::owner_verify_token(duplicate)));
    CHECK(!succeeded(slot_data_api().request_load(bound.handle, session().epoch, &request)));
    slot_data_set_available(false);
    CHECK(g_disk.count({0, "test.immediate"}) == 1);
    std::printf("slot_data_provider: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
