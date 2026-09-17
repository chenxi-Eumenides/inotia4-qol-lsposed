#include "features/world_teleport/plugin.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "core/native/qol_log.h"
#include "features/world_teleport/feature.h"
#include "game/access/world.h"
#include "game_access.h"

// 世界传送入口 patch 机制层（过渡期 legacy；见 plugin.h 说明）。
//
// 三段式 trampoline（近址 mmap，复刻 core/native/call_patch 的页 carving 思路）：
//   ldr x16, #16 ; blr x16 ; mov x0,#1 ; b <return_addr>
// 其中 x16 = world_teleport_call_map_name（模块业务入口），末两条保留原指令 `mov x0,#1`
// 并跳回 epilogue。patch 点地址/回跳地址/原字/改版目标全部经 game/access/world 解析。
namespace qol::features::world_teleport {
namespace {

constexpr size_t kPageSize = 0x1000;
constexpr int64_t kBranchRange = 0x08000000LL;

std::mutex g_world_teleport_mtx;
bool g_installed = false;
void* g_trampoline = nullptr;

bool encode_b(uintptr_t from, uintptr_t to, uint32_t* out) {
    if (from == 0 || to == 0 || out == nullptr) return false;
    const int64_t delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    constexpr int64_t kMin = -(int64_t{1} << 27);
    constexpr int64_t kMax = (int64_t{1} << 27) - 4;
    if ((delta & 0x3) != 0 || delta < kMin || delta > kMax) return false;
    *out = 0x14000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
    return true;
}

bool write_code_word(uintptr_t address, uint32_t word) {
    if (address == 0) return false;
    const uintptr_t page = address & ~(static_cast<uintptr_t>(kPageSize) - 1);
    if (mprotect(reinterpret_cast<void*>(page), kPageSize,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: mprotect failed addr=%p errno=%d",
                      reinterpret_cast<void*>(address), errno);
        return false;
    }
    *reinterpret_cast<uint32_t*>(address) = word;
    __builtin___clear_cache(reinterpret_cast<char*>(address),
                            reinterpret_cast<char*>(address + sizeof(uint32_t)));
    return true;
}

void* allocate_trampoline_near(uintptr_t target) {
#ifndef MAP_FIXED_NOREPLACE
    (void)target;
    return nullptr;
#else
    const uintptr_t base = target & ~(static_cast<uintptr_t>(kPageSize) - 1);
    for (const int64_t step : {int64_t{0x10000}, int64_t{0x1000}}) {
        for (int64_t distance = step; distance < kBranchRange; distance += step) {
            for (const int sign : {1, -1}) {
                const int64_t candidate = static_cast<int64_t>(base) + sign * distance;
                if (candidate <= 0) continue;
                void* region = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(candidate)),
                                    kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                if (region != MAP_FAILED) return region;
            }
        }
    }
    return nullptr;
#endif
}

// 入口 patch 安装（幂等）：成功置 *supported=true。指令内容即版本指纹——原版 mov x0,#1
// 或改版 b 注入块均接受（后者被模块 branch 覆盖，改版注入块整条变死代码）；两者都不匹配
// 时记日志并 safe no-op（*supported 保持 false）。
bool install_entry_patch(bool* supported) {
    *supported = false;
    if (g_trampoline != nullptr) {
        *supported = true;
        return true;
    }
    qol::game::WorldTeleportPatch info{};
    if (!qol::game::world_teleport_patch_info(&info)) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: entry patch info unavailable");
        return false;
    }
    uint32_t legacy_word = 0;
    if (!encode_b(info.patch_addr, info.legacy_target, &legacy_word)) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: legacy branch encoding failed");
        return false;
    }
    const uint32_t original_word = info.expected_word;
    const uint32_t current_word = *reinterpret_cast<const uint32_t*>(info.patch_addr);
    if (current_word != original_word && current_word != legacy_word) {
        QOL_LOG_WARN(QolDomain::kUi,
                     "world teleport: unsupported entry instruction=0x%08x expected mov=0x%08x legacy=0x%08x",
                     current_word, original_word, legacy_word);
        return true;
    }

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = static_cast<long>(kPageSize);
    const size_t length = static_cast<size_t>(page);
    void* region = allocate_trampoline_near(info.patch_addr);
    if (region == nullptr) {
        QOL_LOG_ERROR(QolDomain::kUi,
                      "world teleport: near trampoline mmap failed patch=%p errno=%d",
                      reinterpret_cast<void*>(info.patch_addr), errno);
        return false;
    }
    auto* code = reinterpret_cast<uint8_t*>(region);
    uint32_t return_branch = 0;
    if (!encode_b(reinterpret_cast<uintptr_t>(code) + 12, info.return_addr, &return_branch)) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: trampoline return branch out of range");
        munmap(region, length);
        return false;
    }
    uint32_t patch_branch = 0;
    if (!encode_b(info.patch_addr, reinterpret_cast<uintptr_t>(code), &patch_branch)) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: entry branch out of range");
        munmap(region, length);
        return false;
    }
    const uint32_t trampoline_code[] = {
        0x58000090u, // ldr x16, #16
        0xd63f0200u, // blr x16
        0xd2800020u, // mov x0, #1
        return_branch,
    };
    std::memcpy(code, trampoline_code, sizeof(trampoline_code));
    *reinterpret_cast<uintptr_t*>(code + 16) =
        reinterpret_cast<uintptr_t>(&world_teleport_call_map_name);
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code + 16 + sizeof(uintptr_t)));
    if (!write_code_word(info.patch_addr, patch_branch)) {
        munmap(region, length);
        return false;
    }
    g_trampoline = region;
    *supported = true;
    QOL_LOG_INFO(QolDomain::kUi,
                 "world teleport: entry patched addr=%p old=0x%08x branch=0x%08x trampoline=%p",
                 reinterpret_cast<void*>(info.patch_addr), current_word, patch_branch, region);
    return true;
}

bool install_world_teleport_if_ready() {
    std::lock_guard<std::mutex> lock(g_world_teleport_mtx);
    if (g_installed) return true;
    if (!bridge_ready() || g_base == 0) return false;
    bool supported = false;
    if (!install_entry_patch(&supported)) return false;
    if (!supported) return true;
    if (!world_teleport_native_choice_install()) {
        QOL_LOG_ERROR(QolDomain::kUi, "world teleport: native choice install failed");
        return false;
    }
    g_installed = true;
    return true;
}

constexpr const char* kPluginId = "world_teleport";
constexpr const char* kPluginVersion = "0.1.0";

int plugin_install(contracts::PluginContext& ctx) {
    (void)ctx;
    // 入口 patch 为 legacy 机制（见 plugin.h）；失败只记日志，不影响装机（与迁移前
    // nativeInit 忽略 world_teleport_install_if_ready 返回值一致）。
    install_world_teleport_if_ready();
    return 0;
}

void plugin_deactivate(contracts::PluginContext& ctx) {
    (void)ctx;
    // 零资源插件无宿主托管资源可归还；入口指令 patch 属未托管 legacy 机制，进程内常驻
    // （待 patch/ 支持运行时目标后转声明式资源，由宿主管理）。install 幂等，重启后重装。
}

void plugin_on_enabled(contracts::PluginContext& ctx) {
    (void)ctx;
}

void plugin_on_disabled(contracts::PluginContext& ctx) {
    (void)ctx;
}

void plugin_fill_status(contracts::PluginStatusOut& out) {
    out.enabled = true;  // 零资源、无配置门禁：常开
    out.error = nullptr;
}

const contracts::PluginDescriptor kDescriptor = {
    kPluginId,
    kPluginVersion,
    0, nullptr,   // requires
    0, nullptr,   // optional_requires
    0, nullptr,   // provides
    0, nullptr,   // resources（零资源：入口 patch 为 legacy 机制，见 plugin.h）
    0, nullptr,   // jni
    &plugin_install,
    &plugin_deactivate,
    &plugin_on_enabled,
    &plugin_on_disabled,
    &plugin_fill_status,
    nullptr,      // on_resource_ready（无资源）
};

}  // namespace

const contracts::PluginDescriptor& plugin_descriptor() {
    return kDescriptor;
}

}  // namespace qol::features::world_teleport
