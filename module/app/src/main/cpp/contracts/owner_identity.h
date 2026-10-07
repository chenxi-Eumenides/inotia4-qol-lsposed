#pragma once

#include <cstdint>

#include "owner_token.h"
#include "host_sdk.h"
#include "result.h"

namespace qol::contracts {

// token 由 Host 注入，不接受调用者自报 owner。当前 PluginContext 只有自报可引用的
// plugin_id，仅在 spec 加 owner 字符串不足以落实身份约束，必须宿主注入凭据。
// token 签发属于 Host 装配期职责，不进入此契约；授权由装配期注册的 token 决定，
// 不按 plugin_id 字符串比较或白名单特判插件名称。
// 签发与 query 属 Host 装配期内部；插件可见的只有 verify，
// 仅校验调用方已持有的 token，不提供签发或枚举其他 owner 的能力。
// out_generation 为 owner 生命周期代次，停用后递增，使旧 token 失效。
using QolOwnerQueryFn = QolResult (*)(void* host_ctx, QolOwnerToken* out_token,
                                     uint64_t* out_generation);
using QolOwnerVerifyFn = QolResult (*)(void* host_ctx, QolOwnerToken token);

struct QolOwnerIdentityApi {
    QolAbiHeader abi;
    QolOwnerQueryFn query;
    QolOwnerVerifyFn verify;
};

}  // namespace qol::contracts
