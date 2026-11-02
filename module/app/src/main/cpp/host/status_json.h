#pragma once

#include <cstdint>
#include <string>

// P1 最小诊断接口的数据源：把 infra 状态登记表导出为 JSON。
// 命名空间取 qol::host：该文件属「宿主装配层」，位于 bridge 与 core/feature 之间，
// 供 bridge/native 的 JNI 薄层调用，不反向依赖 JNI。

namespace qol::host {

// 返回 {"ok":true,"features":[...]}；失败返回 {"ok":false,"error":"..."}。
// 首次调用会幂等注册 bootstrap 条目（见 .cpp）。
std::string host_feature_status_json();

// 返回 {"ok":true,"resources":[{id, owner, mechanism, policy, state, in_flight, isolated}, ...]}。
// 资源登记表全量导出（ARCHITECTURE.md §9.1 P3）：state = free/disabled/live/tombstone。
std::string host_resources_json();

// 返回 {"ok":true,"ticket":N,"state":"pending|done|failed|cancelled"}；
// ticket==0 返回 {"ok":false,"error":"invalid ticket"}（ARCHITECTURE.md §9.1 P3）。
std::string host_ticket_json(uint64_t ticket);

}  // namespace qol::host
