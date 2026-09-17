#pragma once

#include <cstdint>

// 属性范围能力契约（ARCHITECTURE.md §3.4）：首个真实 provide/require。
// 只读查询接口；接口对象由提供方保证在撤销事务完成前有效，消费者不得缓存裸指针跨撤销点。

namespace qol::contracts {

constexpr const char* kAttrRangeCapabilityId = "attr_range";
constexpr int32_t kAttrRangeVersionMajor = 1;
constexpr int32_t kAttrRangeVersionMinor = 0;

struct AttrRangeApi {   // POD：只读查询（§3.4 类型限制）
    // 探测 item 的宝石属性随机区间；false = 不可用/不支持（消费者走降级路径）。
    bool (*probe_jewel_range)(int type, void* item, int* out_min, int* out_max);
};

}  // namespace qol::contracts
