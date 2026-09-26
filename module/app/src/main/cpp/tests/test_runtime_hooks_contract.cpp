// T2l-rh：contracts/runtime_hooks.h 的契约面断言（无 host TU 依赖，编译期 + ABI 静态面）。
// 表定义的运行期行为（包装 transition_run/frame_task 的逐字等价）由 host/frame_source.cpp
// 的静态对照与后续写批核验；本 target 只保证 POD/版本/字段签名与帧点位常量不漂移。

#include "contracts/runtime_hooks.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

int main() {
    using qol::contracts::QolRuntimeHooksApi;
    using qol::contracts::QolRuntimeTransitionBodyFn;
    using qol::contracts::QolRuntimeFrameTaskFn;
    using QolResult = ::QolResult;
    using QolVersion = ::QolVersion;

    // POD/ABI 面（contracts 铁律）。
    CHECK(std::is_standard_layout<QolRuntimeHooksApi>::value);
    CHECK(std::is_trivially_copyable<QolRuntimeHooksApi>::value);
    CHECK(alignof(QolRuntimeHooksApi) == alignof(void*));
    CHECK(offsetof(QolRuntimeHooksApi, abi_version) == 0);
    // 首个函数指针按指针对齐落位（abi_version 4B + 对齐填充）。
    CHECK(offsetof(QolRuntimeHooksApi, transition_run) ==
          (alignof(void*) > sizeof(QolVersion) ? alignof(void*) : sizeof(QolVersion)));

    // 版本与帧点位常量（W31：与 infra 同值；infra 侧 static_assert 复核）。
    CHECK(qol::contracts::kQolRuntimeHooksVersion.major == 1);
    CHECK(qol::contracts::kQolRuntimeHooksVersion.minor == 0);
    CHECK(qol::contracts::kQolRuntimeFramePointRenderPre == 0);
    CHECK(qol::contracts::kQolRuntimeFramePointLogicPre == 1);

    // 字段签名（编译期类型校验，防止后续批改写 ABI）。
    static_assert(std::is_same<decltype(QolRuntimeHooksApi::transition_run),
                               QolResult (*)(QolRuntimeTransitionBodyFn, void*, int32_t, char*,
                                             uint32_t, uint32_t*)>::value,
                  "transition_run signature drift");
    static_assert(std::is_same<decltype(QolRuntimeHooksApi::transition_run_inline),
                               QolResult (*)(QolRuntimeTransitionBodyFn, void*, char*, uint32_t,
                                             uint32_t*)>::value,
                  "transition_run_inline signature drift");
    static_assert(std::is_same<decltype(QolRuntimeHooksApi::frame_task_add),
                               uint64_t (*)(int32_t, QolRuntimeFrameTaskFn, void*, int32_t,
                                            int32_t)>::value,
                  "frame_task_add signature drift");
    static_assert(std::is_same<decltype(QolRuntimeHooksApi::frame_task_remove),
                               bool (*)(uint64_t)>::value,
                  "frame_task_remove signature drift");
    static_assert(std::is_same<decltype(QolRuntimeHooksApi::save_enter_mark_pending),
                               void (*)()>::value,
                  "save_enter_mark_pending signature drift");
    static_assert(std::is_same<decltype(QolRuntimeHooksApi::capability_require),
                               const void* (*)(const char*, uint16_t, uint16_t)>::value,
                  "capability_require signature drift");
    static_assert(std::is_same<decltype(&qol::contracts::qol_runtime_hooks),
                               const QolRuntimeHooksApi& (*)()>::value,
                  "accessor signature drift");
    CHECK(true);  // 上述 static_assert 若漂移则编译失败；此处计入运行断言的通过数。

    std::printf("runtime_hooks_contract: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
