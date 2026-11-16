#include "infra/panels.h"

#include <cstring>

#include "contracts/capacity.h"

namespace qol::infra {
namespace {

// 定长声明表：只追加，永不重排/删除/复用槽位。available 与声明分离，
// 使任何持有 &g_providers[i].spec 的调用方在插件启停后仍读到同一声明项。
struct ProviderEntry {
    contracts::PanelProviderSpec spec;
    bool available;
};

ProviderEntry g_providers[qol::capacity::kMaxPanelProviders];
uint32_t g_provider_count = 0;

bool non_empty(const char* text) { return text != nullptr && text[0] != '\0'; }

bool same_id(const contracts::PanelProviderSpec& provider, const char* panel_id) {
    return provider.panel_id != nullptr && panel_id != nullptr &&
           std::strcmp(provider.panel_id, panel_id) == 0;
}

int find_declared_index(const char* panel_id) {
    for (uint32_t i = 0; i < g_provider_count; ++i) {
        if (same_id(g_providers[i].spec, panel_id)) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

const char* panels_validate_declaration(const char* owner,
                                        const contracts::PanelProviderSpec* providers,
                                        uint32_t count) {
    if (!non_empty(owner)) return "provider declaration requires owner";
    if (count > qol::capacity::kMaxPanelProvidersPerPlugin)
        return "provider declaration exceeds per-plugin budget";
    if (count > 0 && providers == nullptr) return "provider declaration array required";
    if (count > qol::capacity::kMaxPanelProviders - g_provider_count)
        return "provider declaration exceeds table capacity";
    for (uint32_t i = 0; i < count; ++i) {
        const contracts::PanelProviderSpec& provider = providers[i];
        if (!non_empty(provider.panel_id)) return "provider panel_id required";
        if (!non_empty(provider.owner)) return "provider owner required";
        if (std::strcmp(provider.owner, owner) != 0) return "provider owner must match plugin id";
        if (provider.open == nullptr || provider.close == nullptr || provider.active == nullptr)
            return "provider callbacks required";
        if (find_declared_index(provider.panel_id) >= 0) return "duplicate provider panel_id";
        for (uint32_t j = 0; j < i; ++j) {
            if (same_id(providers[j], provider.panel_id) ||
                std::strcmp(providers[j].panel_id, provider.panel_id) == 0)
                return "duplicate provider panel_id in declaration";
        }
    }
    return nullptr;
}

bool panels_declare_provider(const contracts::PanelProviderSpec& provider) {
    if (g_provider_count >= qol::capacity::kMaxPanelProviders) return false;
    if (!non_empty(provider.panel_id) || !non_empty(provider.owner)) return false;
    if (provider.open == nullptr || provider.close == nullptr || provider.active == nullptr)
        return false;
    if (find_declared_index(provider.panel_id) >= 0) return false;
    g_providers[g_provider_count].spec = provider;
    g_providers[g_provider_count].available = false;
    ++g_provider_count;
    return true;
}

void panels_set_owner_available(const char* owner, bool available) {
    if (!non_empty(owner)) return;
    for (uint32_t i = 0; i < g_provider_count; ++i) {
        const contracts::PanelProviderSpec& provider = g_providers[i].spec;
        if (provider.owner != nullptr && std::strcmp(provider.owner, owner) == 0) {
            g_providers[i].available = available;
        }
    }
}

uint32_t panels_provider_count() {
    uint32_t count = 0;
    for (uint32_t i = 0; i < g_provider_count; ++i) {
        if (g_providers[i].available) ++count;
    }
    return count;
}

const contracts::PanelProviderSpec* panels_provider_at(uint32_t index) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < g_provider_count; ++i) {
        if (!g_providers[i].available) continue;
        if (seen == index) return &g_providers[i].spec;
        ++seen;
    }
    return nullptr;
}

const contracts::PanelProviderSpec* panels_find_provider(const char* panel_id) {
    if (!non_empty(panel_id)) return nullptr;
    const int index = find_declared_index(panel_id);
    if (index < 0 || !g_providers[index].available) return nullptr;
    return &g_providers[index].spec;
}

void panels_reset_for_test() {
    for (uint32_t i = 0; i < g_provider_count; ++i) {
        g_providers[i].spec = contracts::PanelProviderSpec{};
        g_providers[i].available = false;
    }
    g_provider_count = 0;
}

}  // namespace qol::infra
