#include "stack.h"

#include <cstring>
#include <memory>

#include "host.h"

namespace qol::features::core_ui {
namespace {

const contracts::QolHostSdk* g_sdk = nullptr;
std::unique_ptr<PopupHost> g_popup_host;
const char* g_stack[kPanelStackDepth] = {};
int g_depth = 0;

const contracts::PanelProviderSpec* provider_of(const char* panel_id) {
    if (g_sdk == nullptr || g_sdk->panels.find == nullptr) return nullptr;
    const contracts::PanelProviderSpec* provider = nullptr;
    if (!contracts::succeeded(g_sdk->panels.find(panel_id, &provider))) return nullptr;
    return provider;
}

bool active_of(const char* panel_id) {
    const contracts::PanelProviderSpec* provider = provider_of(panel_id);
    return provider != nullptr && provider->active != nullptr && provider->active();
}

int stack_index_of(const char* panel_id) {
    for (int i = 0; i < g_depth; ++i) {
        if (std::strcmp(g_stack[i], panel_id) == 0) return i;
    }
    return -1;
}

void push(const char* panel_id) {
    if (g_depth < kPanelStackDepth) g_stack[g_depth++] = panel_id;
}

void drop_top() {
    if (g_depth > 0) g_stack[--g_depth] = nullptr;
}

// 关闭栈顶：调 provider.close（幂等）后弹栈。
void close_top() {
    if (g_depth == 0) return;
    const contracts::PanelProviderSpec* provider = provider_of(g_stack[g_depth - 1]);
    if (provider != nullptr && provider->close != nullptr) {
        if (g_popup_host != nullptr && g_popup_host->owns_callbacks(*provider)) {
            (void)g_popup_host->close(provider->panel_id, true);
            (void)g_popup_host->pump(true);
        } else {
            provider->close();
        }
    }
    drop_top();
}

// provider 可能自行关闭（游戏内返回键），栈记录会滞后：只丢记录，不再调 close
// （provider 已自关，重复关闭可能重入游戏 pop 路径）。
void prune_inactive_top() {
    while (g_depth > 0 && !active_of(g_stack[g_depth - 1])) {
        drop_top();
    }
}

}  // namespace

void ui_panels_bind_sdk(const contracts::QolHostSdk* sdk) {
    if (g_popup_host != nullptr) {
        g_popup_host->deactivate(true);
        (void)g_popup_host->pump(true);
        g_popup_host.reset();
    }
    g_sdk = sdk;
    if (sdk != nullptr) {
        g_popup_host = std::make_unique<PopupHost>("F_PANEL_UNK1_ENTER", sdk);
    }
}

bool ui_panels_open(const char* panel_id) {
    if (panel_id == nullptr || panel_id[0] == '\0') return false;
    const contracts::PanelProviderSpec* provider = provider_of(panel_id);
    if (provider == nullptr || provider->open == nullptr) return false;

    prune_inactive_top();
    if (g_depth > 0 && std::strcmp(g_stack[g_depth - 1], panel_id) == 0) {
        return true;  // 已在栈顶且 active（prune 已保证）→ 幂等
    }
    // provider 已 active 但未在本栈（例如更早直接打开）：补记栈。
    if (provider->active != nullptr && provider->active()) {
        push(panel_id);
        return true;
    }
    if (g_depth >= kPanelStackDepth) close_top();
    const bool opened = g_popup_host != nullptr && g_popup_host->owns_callbacks(*provider)
        ? g_popup_host->open(*provider, true)
        : provider->open();
    if (opened) {
        push(panel_id);
        return true;
    }
    // 失败兜底：关栈顶再开。嵌套 push 在游戏 popup 栈满时失败，关掉上层后重试。
    if (g_depth > 0) {
        close_top();
        const bool retry_opened = g_popup_host != nullptr && g_popup_host->owns_callbacks(*provider)
            ? g_popup_host->open(*provider, true)
            : provider->open();
        if (retry_opened) {
            push(panel_id);
            return true;
        }
    }
    return false;
}

void ui_panels_close(const char* panel_id) {
    if (panel_id == nullptr || panel_id[0] == '\0') return;
    const int index = stack_index_of(panel_id);
    if (index >= 0) {
        // 关闭该面板及其之上的全部面板（保持与 UI 栈一致）。
        while (g_depth > index) close_top();
        return;
    }
    const contracts::PanelProviderSpec* provider = provider_of(panel_id);
    if (provider != nullptr && provider->close != nullptr && provider->active != nullptr &&
        provider->active()) {
        if (g_popup_host != nullptr && g_popup_host->owns_callbacks(*provider)) {
            (void)g_popup_host->close(provider->panel_id, true);
            (void)g_popup_host->pump(true);
        } else {
            provider->close();
        }
    }
}

bool ui_panels_active(const char* panel_id) {
    if (panel_id == nullptr || panel_id[0] == '\0') return false;
    return active_of(panel_id);
}

const char* ui_panels_block_reason() {
    // 栈顶优先：嵌套时报告最上层占用（备份面板在设置页之上）。
    for (int i = g_depth - 1; i >= 0; --i) {
        const contracts::PanelProviderSpec* provider = provider_of(g_stack[i]);
        if (provider == nullptr || provider->active == nullptr || !provider->active()) continue;
        if (provider->block_reason != nullptr) {
            const char* reason = provider->block_reason();
            if (reason != nullptr) return reason;
        }
    }
    return nullptr;
}

void ui_panels_reset_for_test() {
    g_depth = 0;
}

int ui_panels_depth_for_test() {
    return g_depth;
}

}  // namespace qol::features::core_ui
