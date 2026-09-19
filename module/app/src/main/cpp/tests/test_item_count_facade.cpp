#include "host/item_count_facade.h"

#include <cassert>
#include <cstdint>

#include "contracts/result.h"

namespace {

int g_calls = 0;

QolResult canonical(const void*, uint32_t* out) {
    ++g_calls;
    *out = 199;
    return qol::contracts::ok();
}
QolResult effective(const void*, uint32_t* out) {
    ++g_calls;
    *out = 999;
    return qol::contracts::ok();
}
QolResult is_encoded(const void*, qol::contracts::QolItemCountEncoding* out) {
    ++g_calls;
    *out = qol::contracts::QolItemCountEncoding::kEncoded;
    return qol::contracts::ok();
}
QolResult write_effective(void*, uint32_t count) {
    ++g_calls;
    assert(count == 321);
    return qol::contracts::ok();
}
QolResult writeback_needed(const void*, uint32_t count, int32_t* out) {
    ++g_calls;
    assert(count == 321);
    *out = 1;
    return qol::contracts::ok();
}

const qol::contracts::QolItemCountApi kProvider = {
    &canonical, &effective, &is_encoded, &write_effective, &writeback_needed};

void assert_not_ready_without_provider() {
    qol::host::host_item_count_set_provider(nullptr);
    uint32_t count = 77;
    auto encoding = qol::contracts::QolItemCountEncoding::kUnknown;
    int32_t needed = 77;
    assert(qol::host::host_item_count_api().canonical(reinterpret_cast<void*>(1), &count).code ==
           kQolErrNotReady);
    assert(qol::host::host_item_count_api().effective(reinterpret_cast<void*>(1), &count).code ==
           kQolErrNotReady);
    assert(qol::host::host_item_count_api().is_encoded(reinterpret_cast<void*>(1), &encoding).code ==
           kQolErrNotReady);
    assert(qol::host::host_item_count_api().write_effective(reinterpret_cast<void*>(1), 1).code ==
           kQolErrNotReady);
    assert(qol::host::host_item_count_api().writeback_needed(reinterpret_cast<void*>(1), 1, &needed).code ==
           kQolErrNotReady);
    assert(count == 77);
    assert(encoding == qol::contracts::QolItemCountEncoding::kUnknown);
    assert(needed == 77);

    assert(qol::host::host_item_count_api().canonical(nullptr, &count).code ==
           kQolErrInvalidArgument);
    assert(qol::host::host_item_count_api().write_effective(nullptr, 1).code ==
           kQolErrInvalidArgument);
    assert(count == 77);
}

QolResult unknown_canonical(const void*, uint32_t*) {
    return qol::contracts::error(kQolErrUnsupported, "unsupported", 11);
}
QolResult unknown_effective(const void*, uint32_t*) {
    return qol::contracts::error(kQolErrUnsupported, "unsupported", 11);
}
QolResult unknown_is_encoded(const void*, qol::contracts::QolItemCountEncoding* out) {
    *out = qol::contracts::QolItemCountEncoding::kUnknown;
    return qol::contracts::ok();
}
QolResult unknown_write(void*, uint32_t) {
    return qol::contracts::error(kQolErrUnsupported, "unsupported", 11);
}
QolResult unknown_writeback(const void*, uint32_t, int32_t*) {
    return qol::contracts::error(kQolErrUnsupported, "unsupported", 11);
}

void assert_unknown_is_not_guessed() {
    const qol::contracts::QolItemCountApi provider = {
        &unknown_canonical, &unknown_effective, &unknown_is_encoded, &unknown_write,
        &unknown_writeback};
    qol::host::host_item_count_set_provider(&provider);
    uint32_t count = 77;
    int32_t needed = 77;
    auto encoding = qol::contracts::QolItemCountEncoding::kEncoded;
    assert(qol::host::host_item_count_api().canonical(reinterpret_cast<void*>(1), &count).code ==
           kQolErrUnsupported);
    assert(qol::host::host_item_count_api().effective(reinterpret_cast<void*>(1), &count).code ==
           kQolErrUnsupported);
    assert(qol::host::host_item_count_api().is_encoded(reinterpret_cast<void*>(1), &encoding).ok ==
           0);
    assert(encoding == qol::contracts::QolItemCountEncoding::kUnknown);
    assert(qol::host::host_item_count_api().write_effective(reinterpret_cast<void*>(1), 999).code ==
           kQolErrUnsupported);
    assert(qol::host::host_item_count_api().writeback_needed(reinterpret_cast<void*>(1), 999,
                                                             &needed).code == kQolErrUnsupported);
    assert(count == 77);
    assert(needed == 77);
    qol::host::host_item_count_set_provider(nullptr);
}

void assert_provider_forwarding_and_null_arguments() {
    g_calls = 0;
    qol::host::host_item_count_set_provider(&kProvider);
    uint32_t count = 0;
    auto encoding = qol::contracts::QolItemCountEncoding::kUnknown;
    int32_t needed = 0;
    assert(qol::host::host_item_count_api().canonical(reinterpret_cast<void*>(1), &count).ok == 0);
    assert(count == 199);
    assert(qol::host::host_item_count_api().effective(reinterpret_cast<void*>(1), &count).ok == 0);
    assert(count == 999);
    assert(qol::host::host_item_count_api().is_encoded(reinterpret_cast<void*>(1), &encoding).ok == 0);
    assert(encoding == qol::contracts::QolItemCountEncoding::kEncoded);
    assert(qol::host::host_item_count_api().write_effective(reinterpret_cast<void*>(1), 321).ok == 0);
    assert(qol::host::host_item_count_api().writeback_needed(reinterpret_cast<void*>(1), 321, &needed).ok == 0);
    assert(needed == 1);
    assert(g_calls == 5);

    count = 77;
    assert(qol::host::host_item_count_api().canonical(nullptr, &count).code ==
           kQolErrInvalidArgument);
    assert(qol::host::host_item_count_api().canonical(reinterpret_cast<void*>(1), nullptr).code ==
           kQolErrInvalidArgument);
    assert(qol::host::host_item_count_api().is_encoded(nullptr, &encoding).code ==
           kQolErrInvalidArgument);
    assert(qol::host::host_item_count_api().write_effective(nullptr, 1).code ==
           kQolErrInvalidArgument);
    assert(qol::host::host_item_count_api().writeback_needed(nullptr, 1, &needed).code ==
           kQolErrInvalidArgument);
    assert(qol::host::host_item_count_api().writeback_needed(reinterpret_cast<void*>(1), 1, nullptr).code ==
           kQolErrInvalidArgument);
    assert(count == 77);

    qol::host::host_item_count_set_provider(nullptr);
}

}  // namespace

int main() {
    assert_not_ready_without_provider();
    assert_unknown_is_not_guessed();
    assert_provider_forwarding_and_null_arguments();
    return 0;
}
