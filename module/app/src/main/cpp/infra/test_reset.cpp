#include "infra/test_reset.h"

#include "infra/capabilities.h"
#include "infra/command_queue.h"
#include "infra/events.h"
#include "infra/status.h"

namespace qol::infra {

// 聚合重置：只做各模块自身清理的转发，不含业务语义。
void reset_for_test() {
    command_queue_reset_for_test();
    events_reset_for_test();
    capabilities_reset_for_test();
    status_reset_for_test();
}

}  // namespace qol::infra
