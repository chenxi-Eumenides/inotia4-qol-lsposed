#include "plugin.h"

#include <atomic>
#include <cstdint>

#include "../../contracts/result.h"
#include "stack_codec_impl.h"

// 堆叠上限：999 功能组的声明式所有者。S2 的 codec 与 facade provider 均在本插件内，
// 资源的物理安装/还原交宿主按 gate 同步。inventory_hooks 持有的消费 wrapper 尚未迁入
// 本组，缺口保留在 ADR-0009 与交付报告中。

namespace qol::features::stack_limit {
namespace {

constexpr const char* kPluginId = "stack_limit";
constexpr const char* kPluginVersion = "0.1.0";
constexpr const char* kGate = "plugin.stack_limit.enabled";
constexpr const char* kReason =
    "999 数量组：S2 布局与数量上限必须整组安装，失败时回落原始 99 视图";

#define STACK_LIMIT_CLAIM(NAME)                                                        \
    {NAME, NAME, contracts::ResourceMechanism::kCallPatch,                             \
     contracts::ResourcePolicy::kPrivate, contracts::ResourceRequirement::kRequired,   \
     contracts::ResourceFailure::kFailClosed, true, kPluginId, kReason, kGate, 120,    \
     nullptr, true}

const contracts::ResourceClaim kResources[] = {
    // 既有 6 条数量 clamp。
    STACK_LIMIT_CLAIM("P_INVEN_MOVE_ITEM_CLAMP_150"),
    STACK_LIMIT_CLAIM("P_INVEN_MOVE_ITEM_CLAMP_158"),
    STACK_LIMIT_CLAIM("P_INVEN_SAVE_ITEM_DIRECT_CLAMP_DC"),
    STACK_LIMIT_CLAIM("P_INVEN_SAVE_ITEM_DIRECT_CLAMP_F0"),
    STACK_LIMIT_CLAIM("P_INVEN_SAVE_ITEM_DATA_CLAMP_7C"),
    STACK_LIMIT_CLAIM("P_INVEN_FIND_SAVE_SLOT_CLAMP_238"),
    // S2 位段缩窄 ×2 + 直接位读重定向 ×10。
    STACK_LIMIT_CLAIM("P_SAVE_SAVE_INVENTORY_BITEND"),
    STACK_LIMIT_CLAIM("P_SAVE_LOAD_INVENTORY_BITEND"),
    STACK_LIMIT_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18D0_ARG"),
    STACK_LIMIT_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18D0_CALL"),
    STACK_LIMIT_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18E8_ARG"),
    STACK_LIMIT_CLAIM("P_UISTORE_BUTTON_SELL_EXE_D18E8_CALL"),
    STACK_LIMIT_CLAIM("P_UISTORE_SELL_ITEM_D26B8_ARG"),
    STACK_LIMIT_CLAIM("P_UISTORE_SELL_ITEM_D26B8_CALL"),
    STACK_LIMIT_CLAIM("P_ITEMSYSTEM_DIVIDE_108458_ARG"),
    STACK_LIMIT_CLAIM("P_ITEMSYSTEM_DIVIDE_108458_CALL"),
    STACK_LIMIT_CLAIM("P_ITEMSYSTEM_DIVIDE_10848C_ARG"),
    STACK_LIMIT_CLAIM("P_ITEMSYSTEM_DIVIDE_10848C_CALL"),
};
#undef STACK_LIMIT_CLAIM

std::atomic<bool> g_provider_resident{false};
std::atomic<bool> g_feature_999_enabled{false};
const contracts::QolGameHost* g_game = nullptr;
const contracts::QolHostSdk* g_sdk = nullptr;

QolResult invalid_argument() {
    static constexpr char kMessage[] = "invalid argument";
    return contracts::error(kQolErrInvalidArgument, kMessage, sizeof(kMessage) - 1);
}

QolResult not_ready() {
    static constexpr char kMessage[] = "not ready";
    return contracts::error(kQolErrNotReady, kMessage, sizeof(kMessage) - 1);
}

QolResult unsupported() {
    static constexpr char kMessage[] = "unsupported";
    return contracts::error(kQolErrUnsupported, kMessage, sizeof(kMessage) - 1);
}

bool provider_ready() {
    return g_provider_resident.load(std::memory_order_acquire) && g_game != nullptr;
}

bool gate_enabled() {
    if (g_sdk == nullptr || g_sdk->config.get_bool == nullptr) return false;
    int32_t enabled = 0;
    const QolResult result = g_sdk->config.get_bool(kGate, 0, &enabled);
    return result.ok == kQolOk && enabled != 0;
}

bool atomic_group_satisfied() {
    return g_feature_999_enabled.load(std::memory_order_acquire);
}

bool feature_999_usable() { return gate_enabled() && atomic_group_satisfied(); }

bool gate_wanted_but_group_unready() {
    return provider_ready() && gate_enabled() && !atomic_group_satisfied();
}

contracts::QolItemCountEncoding item_encoding(const void* item) {
    return impl::item_count_encoding(item);
}

QolResult provider_field_from_canonical(uint32_t field, uint32_t canonical, int32_t extended,
                                        uint32_t* out_field) {
    if (out_field == nullptr) return invalid_argument();
    if (!provider_ready()) return not_ready();
    *out_field = impl::field_from_canonical(field, canonical, extended != 0);
    return contracts::ok();
}

QolResult provider_canonical_from_field(uint32_t field, uint32_t* out_canonical) {
    if (out_canonical == nullptr) return invalid_argument();
    if (!provider_ready()) return not_ready();
    *out_canonical = impl::canonical_from_field(field);
    return contracts::ok();
}

QolResult provider_view_from_canonical(uint32_t canonical, int32_t extended, uint32_t* out_view) {
    if (out_view == nullptr) return invalid_argument();
    if (!provider_ready()) return not_ready();
    *out_view = impl::view_from_canonical(canonical, extended != 0);
    return contracts::ok();
}

QolResult provider_view_limit(int32_t extended, uint32_t* out_limit) {
    if (out_limit == nullptr) return invalid_argument();
    if (!provider_ready()) return not_ready();
    *out_limit = impl::view_limit(extended != 0);
    return contracts::ok();
}

QolResult provider_read_encoded_field(const void* item, uint32_t* out_field) {
    if (item == nullptr || out_field == nullptr) return invalid_argument();
    if (!provider_ready()) return not_ready();
    return impl::read_item_count_field(item, out_field) ? contracts::ok() : not_ready();
}

QolResult provider_write_encoded_field(void* item, uint32_t field) {
    if (item == nullptr) return invalid_argument();
    if (!provider_ready()) return not_ready();
    return impl::write_item_count_field(item, field) ? contracts::ok() : not_ready();
}

QolResult canonical_impl(const void* item, uint32_t* out_count) {
    if (!provider_ready()) return not_ready();
    const contracts::QolItemCountEncoding encoding = item_encoding(item);
    if (encoding == contracts::QolItemCountEncoding::kUnknown) {
        return unsupported();
    }
    if (encoding == contracts::QolItemCountEncoding::kEncoded) {
        uint32_t raw = 0;
        if (!impl::read_item_count_field(item, &raw)) return not_ready();
        *out_count = impl::s2_read_count(raw);
        return contracts::ok();
    }
    // ITEM_GetCumulateCount 对非编码类别的原版结果恒为 1。这里不能再次调用
    // Game Host 的 getter：inventory_hooks 的 getter wrapper 本身经 facade 回到本函数，
    // 会形成递归；非编码类别直接保留原版归一化结果。
    *out_count = 1;
    return contracts::ok();
}

QolResult provider_canonical(const void* item, uint32_t* out_count) {
    if (item == nullptr || out_count == nullptr) return invalid_argument();
    return canonical_impl(item, out_count);
}

QolResult provider_effective(const void* item, uint32_t* out_count) {
    if (item == nullptr || out_count == nullptr) return invalid_argument();
    if (!provider_ready()) return not_ready();
    if (gate_wanted_but_group_unready()) return not_ready();
    const contracts::QolItemCountEncoding encoding = item_encoding(item);
    if (encoding == contracts::QolItemCountEncoding::kUnknown) {
        return unsupported();
    }
    if (encoding != contracts::QolItemCountEncoding::kEncoded) {
        return canonical_impl(item, out_count);
    }
    uint32_t canonical = 0;
    const QolResult result = canonical_impl(item, &canonical);
    if (!contracts::succeeded(result)) return result;
    const bool usable = feature_999_usable();
    *out_count = impl::effective_clamp(impl::effective_view_count(canonical, usable), usable);
    return contracts::ok();
}

QolResult provider_is_encoded(const void* item, contracts::QolItemCountEncoding* out_encoding) {
    if (item == nullptr || out_encoding == nullptr) return invalid_argument();
    if (!provider_ready()) return not_ready();
    *out_encoding = item_encoding(item);
    return contracts::ok();
}

QolResult provider_write_effective(void* item, uint32_t count) {
    if (item == nullptr) return invalid_argument();
    if (!provider_ready()) return not_ready();
    if (gate_wanted_but_group_unready()) return not_ready();
    const contracts::QolItemCountEncoding encoding = item_encoding(item);
    if (encoding == contracts::QolItemCountEncoding::kUnknown) {
        return unsupported();
    }
    if (encoding != contracts::QolItemCountEncoding::kEncoded) {
        return unsupported();
    }
    const bool usable = feature_999_usable();
    uint32_t raw = 0;
    if (!impl::read_item_count_field(item, &raw)) return not_ready();
    const uint32_t clamped = impl::effective_clamp(count, usable);
    const uint32_t written = impl::field_from_canonical(raw, clamped, usable);
    return impl::write_item_count_field(item, written) ? contracts::ok() : not_ready();
}

QolResult provider_writeback_needed(const void* item, uint32_t count, int32_t* out_needed) {
    if (item == nullptr || out_needed == nullptr) return invalid_argument();
    if (!provider_ready()) return not_ready();
    if (gate_wanted_but_group_unready()) return not_ready();
    const contracts::QolItemCountEncoding encoding = item_encoding(item);
    if (encoding == contracts::QolItemCountEncoding::kUnknown) {
        return unsupported();
    }
    if (encoding != contracts::QolItemCountEncoding::kEncoded) {
        *out_needed = 0;
        return contracts::ok();
    }
    uint32_t current = 0;
    const QolResult result = provider_effective(item, &current);
    if (!contracts::succeeded(result)) return result;
    const bool usable = feature_999_usable();
    *out_needed = current != impl::effective_clamp(count, usable) ? 1 : 0;
    return contracts::ok();
}

const contracts::QolItemCountApi kItemCountApi = {
    &provider_canonical,
    &provider_effective,
    &provider_is_encoded,
    &provider_write_effective,
    &provider_writeback_needed,
    &provider_field_from_canonical,
    &provider_canonical_from_field,
    &provider_view_from_canonical,
    &provider_view_limit,
    &provider_read_encoded_field,
    &provider_write_encoded_field,
};

// 自身初始化：只缓存 Host ABI，资源仍由宿主按原子组安装。
int plugin_install(contracts::PluginContext& ctx) {
    if (ctx.struct_size < sizeof(contracts::PluginContext) || ctx.sdk == nullptr ||
        ctx.game == nullptr) {
        return 1;
    }
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_provider_resident.store(true, std::memory_order_release);
    host::host_item_count_set_provider(&kItemCountApi);
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    // provider 代码与 item_count 解释面在进程内保持驻留；已有 S2 数据没有迁移方案，
    // 不能在普通 deactivate 后卸载其解释器。deactivate 只撤销 999 功能组的有效态。
    g_feature_999_enabled.store(false, std::memory_order_release);
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    g_sdk = ctx.sdk;
    g_game = ctx.game;
    g_feature_999_enabled.store(true, std::memory_order_release);
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
    // provider 跟随 install/deactivate 驻留；禁用只撤销 999 有效态，canonical 仍可读。
    g_feature_999_enabled.store(false, std::memory_order_release);
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = feature_enabled();
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    static_cast<uint32_t>(sizeof(kResources) / sizeof(kResources[0])), kResources,
    0, nullptr,   // jni（开关经命令端口下发）
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（非入口机制，无 backup 交还）
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

bool provider_resident() {
    return g_provider_resident.load(std::memory_order_acquire) && g_game != nullptr;
}

bool feature_enabled() {
    return provider_resident() && g_feature_999_enabled.load(std::memory_order_acquire);
}

}  // namespace qol::features::stack_limit
