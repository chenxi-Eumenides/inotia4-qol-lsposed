#include "stack.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "../../../contracts/ui.h"
#include "host.h"

namespace qol::features::core_ui {
namespace {

const contracts::QolHostSdk* g_sdk = nullptr;
const contracts::QolHostSdk* g_pending_sdk = nullptr;
std::unique_ptr<PopupHost> g_popup_host;
const char* g_stack[kPanelStackDepth] = {};
const contracts::PanelProviderSpec* g_stack_providers[kPanelStackDepth] = {};
int g_depth = 0;
uint64_t g_pump_task = 0;
uint64_t g_panel_snapshot_sequence = 0;
bool g_unbind_pending = false;
bool g_unbind_host_deactivate_started = false;
bool g_back_requested = false;

void remove_pump_task() {
    if (g_pump_task != 0 && g_sdk != nullptr && g_sdk->scheduler.remove != nullptr) {
        (void)g_sdk->scheduler.remove(g_pump_task);
    }
    g_pump_task = 0;
}

const contracts::PanelProviderSpec* provider_of(const char* panel_id) {
    if (g_sdk == nullptr || g_sdk->panels.find == nullptr) return nullptr;
    const contracts::PanelProviderSpec* provider = nullptr;
    if (!contracts::succeeded(g_sdk->panels.find(panel_id, &provider))) return nullptr;
    return provider;
}

const contracts::PanelProviderSpec* provider_at(int index) {
    if (index < 0 || index >= g_depth) return nullptr;
    if (g_stack_providers[index] != nullptr) return g_stack_providers[index];
    return provider_of(g_stack[index]);
}

bool popup_close_pending_for_top() {
    if (g_popup_host == nullptr) return false;
    if (g_popup_host->close_pending_for_test()) return true;
    if (g_depth == 0) return false;
    const contracts::PanelProviderSpec* provider = provider_at(g_depth - 1);
    return provider != nullptr && g_popup_host->owns_callbacks(*provider) &&
           g_popup_host->top_is(provider->panel_id);
}

// 真实主线程判定：以 host UI 适配表为准，取代编译期常量，避免绕过 adapter 的 pthread 门。
// 适配表未提供该字段（旧宿主/测试桩）时回退为调用方保证。
bool main_thread_now() {
    if (g_sdk == nullptr || g_sdk->ui == nullptr || g_sdk->ui->is_main_thread == nullptr) return true;
    return g_sdk->ui->is_main_thread() != 0;
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

void publish_stack_snapshot() {
    if (g_depth < 0 || g_depth > kPanelStackDepth) return;
    char copied_ids[kPanelStackDepth][contracts::kUiStateMaxPanelIdBytes]{};
    const char* panel_ids[kPanelStackDepth]{};
    for (int i = 0; i < g_depth; ++i) {
        if (g_stack[i] == nullptr ||
            std::strlen(g_stack[i]) >= contracts::kUiStateMaxPanelIdBytes) return;
        std::snprintf(copied_ids[i], sizeof(copied_ids[i]), "%s", g_stack[i]);
        panel_ids[i] = copied_ids[i];
    }
    const uint64_t sequence = ++g_panel_snapshot_sequence;
    if (g_sdk == nullptr || g_sdk->capabilities.require == nullptr) return;
    const void* iface = nullptr;
    if (!contracts::succeeded(g_sdk->capabilities.require(
            contracts::kUiStateCapabilityId,
            {contracts::kUiStateVersionMajor, contracts::kUiStateVersionMinor}, &iface)) ||
        iface == nullptr) return;
    const auto* api = static_cast<const contracts::UiStateApi*>(iface);
    if (api->publish_custom_panels == nullptr) return;
    const contracts::UiCustomPanelsSnapshot snapshot{
        sequence, static_cast<uint32_t>(g_depth), g_depth > 0 ? panel_ids : nullptr};
    (void)api->publish_custom_panels(&snapshot);
}

bool push(const contracts::PanelProviderSpec* provider) {
    if (provider == nullptr || provider->panel_id == nullptr) return false;
    if (g_depth >= kPanelStackDepth) return false;
    g_stack[g_depth] = provider->panel_id;
    g_stack_providers[g_depth] = provider;
    ++g_depth;
    publish_stack_snapshot();
    return true;
}

void drop_top() {
    if (g_depth > 0) {
        --g_depth;
        g_stack[g_depth] = nullptr;
        g_stack_providers[g_depth] = nullptr;
        publish_stack_snapshot();
    }
}

// 关闭栈顶：调 provider.close（幂等）后弹栈。
bool close_top() {
    if (g_depth == 0) return false;
    const contracts::PanelProviderSpec* provider = provider_at(g_depth - 1);
    bool closed = false;
    if (provider != nullptr && provider->close != nullptr) {
        if (g_popup_host != nullptr && g_popup_host->owns_callbacks(*provider)) {
            closed = g_popup_host->close(provider->panel_id, main_thread_now());
            // 延迟关闭（快照失配排队）时宿主内部栈顶仍是本面板：不得提前 drop 栈记录，
            // 否则 g_depth 归 0 而面板仍在，后续点击在 g_depth==0 被静默吞掉（退出失效）。
            // 排队期间重复请求只是重排队（不重复发 pop），完成由帧泵驱动宿主重试推进。
            if (closed && (g_popup_host->top_is(provider->panel_id) ||
                           g_popup_host->close_pending_for_test())) return false;
        } else {
            provider->close();
            closed = provider->active == nullptr || !provider->active();
        }
    }
    if (!closed) {
        g_back_requested = true;
        return false;
    }
    drop_top();
    return true;
}

// provider 可能自行关闭（游戏内返回键），栈记录会滞后：只丢记录，不再调 close
// （provider 已自关，重复关闭可能重入游戏 pop 路径）。
void prune_inactive_top() {
    while (g_depth > 0) {
        const contracts::PanelProviderSpec* provider = provider_at(g_depth - 1);
        if (provider == nullptr || provider->active == nullptr || provider->active() ||
            popup_close_pending_for_top()) break;
        drop_top();
    }
}

bool drain_stack_for_unbind() {
    while (g_depth > 0) {
        if (!close_top()) {
            if (g_popup_host != nullptr) (void)g_popup_host->pump(main_thread_now());
            return false;
        }
    }
    if (g_popup_host != nullptr && !g_unbind_host_deactivate_started) {
        g_popup_host->deactivate(main_thread_now());
        g_unbind_host_deactivate_started = true;
    }
    if (g_popup_host != nullptr && g_popup_host->close_pending_for_test()) {
        (void)g_popup_host->pump(main_thread_now());
        return !g_popup_host->close_pending_for_test();
    }
    return true;
}

void initialize_sdk(const contracts::QolHostSdk* sdk) {
    g_sdk = sdk;
    g_unbind_pending = false;
    g_unbind_host_deactivate_started = false;
    g_pending_sdk = nullptr;
    if (sdk == nullptr) return;
    g_popup_host = std::make_unique<PopupHost>("F_PANEL_UNK1_ENTER", sdk);
    if (sdk->scheduler.add != nullptr && g_pump_task == 0) {
        (void)sdk->scheduler.add(1, &ui_panels_frame_pump, nullptr, 1, 0, &g_pump_task);
    }
    publish_stack_snapshot();
}

void finish_unbind() {
    const contracts::QolHostSdk* next_sdk = g_pending_sdk;
    remove_pump_task();
    g_popup_host.reset();
    g_sdk = nullptr;
    g_back_requested = false;
    initialize_sdk(next_sdk);
}

}  // namespace

void ui_panels_bind_sdk(const contracts::QolHostSdk* sdk) {
    if (g_unbind_pending) {
        g_pending_sdk = sdk;
        return;
    }
    if (g_sdk == nullptr) {
        initialize_sdk(sdk);
        return;
    }
    if (g_sdk == sdk) return;

    g_pending_sdk = sdk;
    g_unbind_pending = true;
    g_unbind_host_deactivate_started = false;
    if (drain_stack_for_unbind()) finish_unbind();
}

int32_t ui_panels_frame_pump(int64_t, void*) {
    // provider 可能自关（存档备份返回键自调 set_process_info(3,0)，不经本栈）：
    // 帧泵开头剔除陈旧栈顶，自关页面静置后 depth 即回落到真实值，
    // 不必等下一次 open/back（auto 导航按钮绘制据此选 X/箭头）。
    prune_inactive_top();
    if (g_unbind_pending) {
        if (drain_stack_for_unbind()) finish_unbind();
        return 1;
    }
    // 帧边界消费延迟关闭请求：此处不在弹窗事件/原生控件遍历内，关闭可安全执行。
    if (g_back_requested) {
        g_back_requested = false;
        (void)ui_panels_back();
    }
    if (g_popup_host != nullptr) (void)g_popup_host->pump(main_thread_now());
    prune_inactive_top();
    return 1;  // 常驻：宿主空闲归还/原版回调 restore 只能由本帧任务推进
}

bool ui_panels_open(const char* panel_id) {
    if (g_unbind_pending || panel_id == nullptr || panel_id[0] == '\0') return false;
    const contracts::PanelProviderSpec* provider = provider_of(panel_id);
    if (provider == nullptr || provider->open == nullptr) return false;
    if (provider->owner != nullptr && g_popup_host != nullptr &&
        g_popup_host->pending_deactivate_for_owner(provider->owner)) return false;

    prune_inactive_top();
    const int existing_index = stack_index_of(panel_id);
    if (existing_index >= 0 && g_stack_providers[existing_index] != provider) {
        while (g_depth > existing_index) {
            if (!close_top()) return false;
        }
    }
    if (g_depth > 0 && std::strcmp(g_stack[g_depth - 1], panel_id) == 0 &&
        g_stack_providers[g_depth - 1] == provider) {
        return true;  // 已在栈顶且 active（prune 已保证）→ 幂等
    }
    // provider 已 active 但未在本栈（例如更早直接打开）：补记栈。
    if (provider->active != nullptr && provider->active()) {
        return push(provider);
    }
    if (g_depth >= kPanelStackDepth && !close_top()) return false;
    const bool opened = g_popup_host != nullptr && g_popup_host->owns_callbacks(*provider)
        ? g_popup_host->open(*provider, main_thread_now())
        : provider->open();
    if (opened) {
        return push(provider);
    }
    // 失败兜底：关栈顶再开。嵌套 push 在游戏 popup 栈满时失败，关掉上层后重试。
    if (g_depth > 0) {
        if (!close_top()) return false;
        const bool retry_opened = g_popup_host != nullptr && g_popup_host->owns_callbacks(*provider)
            ? g_popup_host->open(*provider, main_thread_now())
            : provider->open();
        if (retry_opened) {
            return push(provider);
        }
    }
    return false;
}

void ui_panels_close(const char* panel_id) {
    if (panel_id == nullptr || panel_id[0] == '\0') return;
    const int index = stack_index_of(panel_id);
    if (index >= 0) {
        // 关闭该面板及其之上的全部面板（保持与 UI 栈一致）。
        while (g_depth > index && close_top()) {}
        return;
    }
    const contracts::PanelProviderSpec* provider = provider_of(panel_id);
    if (provider != nullptr && provider->close != nullptr && provider->active != nullptr &&
        provider->active()) {
        if (g_popup_host != nullptr && g_popup_host->owns_callbacks(*provider)) {
            (void)g_popup_host->close(provider->panel_id, main_thread_now());
        } else {
            provider->close();
        }
    }
}

bool ui_panels_back() {
    prune_inactive_top();
    return close_top();
}

void ui_panels_request_back() { g_back_requested = true; }

bool ui_panels_deactivate_owner(const char* owner) {
    if (owner == nullptr || owner[0] == '\0') return false;
    const bool host_complete = g_popup_host == nullptr ||
                               g_popup_host->deactivate_provider(owner, main_thread_now());
    if (g_popup_host != nullptr && g_popup_host->pending_deactivate_for_owner(owner)) {
        return false;
    }
    if (!host_complete) return false;

    bool changed = false;
    bool pending_close = false;
    for (int i = 0; i < g_depth;) {
        const contracts::PanelProviderSpec* provider = provider_at(i);
        if (provider == nullptr) {
            pending_close = true;
            ++i;
            continue;
        }
        if (provider != nullptr && provider->owner != nullptr &&
            std::strcmp(provider->owner, owner) == 0) {
            if (provider->active != nullptr && provider->active()) {
                if (provider->close == nullptr ||
                    (g_popup_host != nullptr && g_popup_host->owns_callbacks(*provider))) {
                    pending_close = true;
                    ++i;
                    continue;
                }
                provider->close();
                if (provider->active()) {
                    pending_close = true;
                    ++i;
                    continue;
                }
            }
            for (int j = i; j + 1 < g_depth; ++j) {
                g_stack[j] = g_stack[j + 1];
                g_stack_providers[j] = g_stack_providers[j + 1];
            }
            g_stack[--g_depth] = nullptr;
            g_stack_providers[g_depth] = nullptr;
            changed = true;
        } else {
            ++i;
        }
    }
    if (changed) publish_stack_snapshot();
    return host_complete && !pending_close;
}

bool ui_panels_active(const char* panel_id) {
    if (panel_id == nullptr || panel_id[0] == '\0') return false;
    return active_of(panel_id);
}

const char* ui_panels_block_reason() {
    // 栈顶优先：嵌套时报告最上层占用（备份面板在设置页之上）。
    for (int i = g_depth - 1; i >= 0; --i) {
        const contracts::PanelProviderSpec* provider = provider_at(i);
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
    for (const char*& panel_id : g_stack) panel_id = nullptr;
    for (const contracts::PanelProviderSpec*& provider : g_stack_providers) provider = nullptr;
    publish_stack_snapshot();
}

int ui_panels_depth_for_test() {
    return g_depth;
}

int ui_panels_depth() {
    return g_depth;
}

void ui_panels_set_depth_for_test(int depth) {
    if (depth >= 0 && depth <= kPanelStackDepth) g_depth = depth;
}

}  // namespace qol::features::core_ui
