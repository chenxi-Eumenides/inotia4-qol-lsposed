#include "infra/panels.h"

#include <cstring>

namespace qol::infra {
namespace {

constexpr uint32_t kMaxItems = 32;
constexpr uint32_t kMaxProviders = 16;

contracts::PanelItemSpec g_items[kMaxItems];
uint32_t g_item_count = 0;

contracts::PanelProviderSpec g_providers[kMaxProviders];
uint32_t g_provider_count = 0;

bool non_empty(const char* text) {
    return text != nullptr && text[0] != '\0';
}

int find_item(const char* id) {
    for (uint32_t i = 0; i < g_item_count; ++i) {
        if (std::strcmp(g_items[i].id, id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int find_provider_index(const char* panel_id) {
    for (uint32_t i = 0; i < g_provider_count; ++i) {
        if (std::strcmp(g_providers[i].panel_id, panel_id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

}  // namespace

bool panels_register_item(const contracts::PanelItemSpec& item) {
    if (!non_empty(item.id) || !non_empty(item.owner)) return false;
    const int index = find_item(item.id);
    if (index >= 0) {
        // 一 id 一 owner：同 owner 幂等（保留首次登记），异 owner 拒绝。
        return std::strcmp(g_items[index].owner, item.owner) == 0;
    }
    if (g_item_count >= kMaxItems) return false;
    g_items[g_item_count++] = item;
    return true;
}

bool panels_register_provider(const contracts::PanelProviderSpec& provider) {
    if (!non_empty(provider.panel_id) || !non_empty(provider.owner)) return false;
    const int index = find_provider_index(provider.panel_id);
    if (index >= 0) {
        return std::strcmp(g_providers[index].owner, provider.owner) == 0;
    }
    if (g_provider_count >= kMaxProviders) return false;
    g_providers[g_provider_count++] = provider;
    return true;
}

void panels_unregister_owner(const char* owner) {
    if (!non_empty(owner)) return;
    // 保序删除（列表顺序对设置页展示稳定）。
    for (uint32_t i = 0; i < g_item_count;) {
        if (std::strcmp(g_items[i].owner, owner) == 0) {
            for (uint32_t j = i + 1; j < g_item_count; ++j) {
                g_items[j - 1] = g_items[j];
            }
            --g_item_count;
            g_items[g_item_count] = contracts::PanelItemSpec{};
        } else {
            ++i;
        }
    }
    for (uint32_t i = 0; i < g_provider_count;) {
        if (std::strcmp(g_providers[i].owner, owner) == 0) {
            for (uint32_t j = i + 1; j < g_provider_count; ++j) {
                g_providers[j - 1] = g_providers[j];
            }
            --g_provider_count;
            g_providers[g_provider_count] = contracts::PanelProviderSpec{};
        } else {
            ++i;
        }
    }
}

uint32_t panels_item_count() {
    return g_item_count;
}

const contracts::PanelItemSpec* panels_item_at(uint32_t index) {
    return index < g_item_count ? &g_items[index] : nullptr;
}

const contracts::PanelProviderSpec* panels_find_provider(const char* panel_id) {
    if (!non_empty(panel_id)) return nullptr;
    const int index = find_provider_index(panel_id);
    return index >= 0 ? &g_providers[index] : nullptr;
}

void panels_reset_for_test() {
    g_item_count = 0;
    g_provider_count = 0;
}

}  // namespace qol::infra
