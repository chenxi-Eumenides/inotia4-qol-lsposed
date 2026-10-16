#include "feature.h"

#include <cstdint>
#include <cstring>

#include "charinfo_scope.h"

namespace qol::features::character_panel_i18n {
namespace {

const contracts::QolHostSdk* g_sdk = nullptr;
thread_local int32_t g_active_scope = contracts::kTextScopeNone;
const contracts::TextLayerApi* g_text_layer = nullptr;
// 表消费者句柄（install 注册；0 = 未注册）。
uint64_t g_consumer = 0;

}  // namespace

void bind_sdk(const contracts::QolHostSdk* sdk) { g_sdk = sdk; }

const contracts::QolHostSdk* host_sdk() { return g_sdk; }

int32_t active_scope_value() { return g_active_scope; }

void set_active_scope_value(int32_t value) { g_active_scope = value; }

const char* owner_table_modify(uint32_t text_id, const char* current, void* ctx) {
    (void)current;
    (void)ctx;
    if (text_id > 0xFFFFu) return nullptr;
    // 命中返回模块字面量；未命中/占位 nullptr 返回 nullptr（不改）。
    return lookup(static_cast<uint16_t>(text_id), g_active_scope);
}

bool register_consumers() {
    if (g_consumer != 0) return true;  // 幂等（activate 重入）
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return false;
    const void* iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kTextLayerCapabilityId,
            {contracts::kTextLayerVersionMajor, contracts::kTextLayerVersionMinor}, &iface)) ||
        iface == nullptr) {
        return false;
    }
    g_text_layer = static_cast<const contracts::TextLayerApi*>(iface);
    if (g_text_layer->subscribe == nullptr) {
        g_text_layer = nullptr;
        return false;
    }
    const uint64_t id = g_text_layer->subscribe(static_cast<int32_t>(kOwnerTablePhase),
                                                static_cast<int32_t>(kOwnerTableKind),
                                                &owner_table_modify, nullptr);
    if (id == 0) {
        g_text_layer = nullptr;
        return false;
    }
    g_consumer = id;
    return true;
}

void unregister_consumers() {
    if (g_consumer == 0 || g_text_layer == nullptr) return;
    if (g_text_layer->unsubscribe != nullptr) g_text_layer->unsubscribe(g_consumer);
    g_consumer = 0;
    g_text_layer = nullptr;
}

}  // namespace qol::features::character_panel_i18n
