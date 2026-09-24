#include "contracts/extension_bag.h"
#include "contracts/plugin.h"
#include "game/state/item_move_capability.h"

#include <cassert>
#include <cstring>

namespace {

bool g_enabled = true;
int g_move_calls = 0;
int g_fallback_calls = 0;

bool enabled() { return g_enabled; }

int32_t move_item(int32_t, int32_t, int32_t, int32_t, char* out, uint32_t capacity,
                  uint32_t* out_len) {
    ++g_move_calls;
    constexpr char kResult[] = "{\"ok\":true}";
    if (out == nullptr || out_len == nullptr || capacity < sizeof(kResult) - 1) return 0;
    std::memcpy(out, kResult, sizeof(kResult) - 1);
    *out_len = sizeof(kResult) - 1;
    return 1;
}

const qol::contracts::ExtensionBagApi make_published_api() {
    qol::contracts::ExtensionBagApi api{};
    api.enabled = &enabled;
    api.move_item = &move_item;
    return api;
}

const qol::contracts::ExtensionBagApi kPublishedApi = make_published_api();

void dispatch_move(const qol::contracts::ExtensionBagApi* api) {
    if (qol::game::extension_bag_move_capability_ready(api)) {
        char out[32]{};
        uint32_t out_len = 0;
        assert(api->move_item(6, 0, 7, 1, out, sizeof(out), &out_len) == 1);
        assert(out_len == std::strlen("{\"ok\":true}"));
    } else {
        ++g_fallback_calls;
    }
}

}  // namespace

int main() {
    const qol::contracts::CapabilityProvisionRef provision{
        qol::contracts::kExtensionBagCapabilityId,
        {static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMajor),
         static_cast<uint16_t>(qol::contracts::kExtensionBagVersionMinor)},
        &kPublishedApi,
    };
    assert(provision.version.major == 2);
    assert(provision.iface == &kPublishedApi);
    dispatch_move(&kPublishedApi);
    assert(g_move_calls == 1 && g_fallback_calls == 0);

    dispatch_move(nullptr);
    assert(g_move_calls == 1 && g_fallback_calls == 1);
    g_enabled = false;
    dispatch_move(&kPublishedApi);
    assert(g_move_calls == 1 && g_fallback_calls == 2);
    return 0;
}
