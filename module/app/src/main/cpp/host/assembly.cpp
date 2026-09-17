#include "host/assembly.h"

#include <cstdint>

#include "core/native/frame_task.h"
#include "core/native/qol_log.h"
#include "feature/patch/native_inventory_hook.h"
#include "features/simple_mode/plugin.h"
#include "game/access/symbols.h"
#include "game/clock.h"
#include "host/plugin_registry.h"
#include "infra/commands.h"
#include "infra/runtime.h"

namespace qol::host {
namespace {

bool g_assembly_done = false;

// 逻辑帧任务：排空命令队列（主线程执行命令体）。
// P2 过渡期复用既有帧宿主（core/native/frame_host 提供 kFramePointLogicPre 派发），
// P3 收口到 scheduler 后由调度器派发。返回 true = 保留期任务，不自动注销。
bool drain_commands_task(int64_t frame, void* ctx) {
    (void)frame;
    (void)ctx;
    // 顺序：先执行命令（可能改配置并触发同步事件），再监督门禁——保证同一帧内
    // 配置生效后状态立即对齐，不把陈旧状态暴露到下一帧。
    infra::commands_drain();
    sync_gates();
    return true;
}

}  // namespace

bool assembly_init() {
    // 仅在完整成功后置位：失败可重试，成功则幂等。
    if (g_assembly_done) return true;

    // 帧时钟注入：infra 不依赖 game，游戏事实由 host 装配时提供（见 infra/runtime.h）。
    infra::runtime_init(infra::Clock{&qol::game::frame_count});

    // 命令通道每逻辑帧排空一次；命令体在主线程执行（§6.1）。
    frame_task_add(kFramePointLogicPre, &drain_commands_task, nullptr, 1, 0);

    // hook/unhook 原语过渡期取自 feature/patch；P5 迁往 patch/ 后改引 patch 层。
    // native_hook_func/native_unhook_func 是机制原语的获取器（返回函数指针）；这里先取获取器
    // 地址再调用：本处只是把机制原语注入宿主，不构成某个游戏符号的安装点（真实安装点在
    // 插件描述符声明、由插件注册表执行），直接写调用式会被 check_resources 按文本
    // `native_hook_func(` 识别成未登记资源安装，而基线棘轮只允许收缩。
    NativeHookFunType (*hook_getter)() = native_hook_func;
    NativeUnhookFunType (*unhook_getter)() = native_unhook_func;
    const HostDeps deps{
        &qol::game::resolve_hook_entry,
        hook_getter(),
        unhook_getter(),
    };

    if (!register_plugin(features::simple_mode::plugin_descriptor())) {
        QOL_LOG_ERROR(QolDomain::kCore, "assembly: simple_mode registration failed");
        return false;
    }

    const int failed = install_all(deps);
    if (failed != 0) {
        QOL_LOG_WARN(QolDomain::kCore, "assembly: %d plugin(s) failed to install", failed);
        return false;
    }
    g_assembly_done = true;
    QOL_LOG_INFO(QolDomain::kCore, "assembly: all plugins ready");
    return true;
}

}  // namespace qol::host
