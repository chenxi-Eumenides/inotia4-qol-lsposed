#pragma once

#include "contracts/events.h"
#include "contracts/host_sdk.h"

namespace qol::contracts { using ::QolResult; }

namespace qol::infra {

void event_state_set_owner_gate(contracts::QolResult (*verify)(contracts::QolOwnerToken));
void event_state_set_main_thread_gate(bool (*gate)());
contracts::QolResult event_state_declare(contracts::QolOwnerToken owner,
                                         const contracts::EventStateDescriptor& descriptor);
contracts::QolResult event_state_set(contracts::QolOwnerToken owner,
                                     const contracts::EventStateValue& value);
contracts::QolResult event_state_read(uint32_t event_id, contracts::EventStateValue* out_value);
contracts::QolResult event_state_subscribe(uint32_t event_id,
                                           contracts::QolEventStateHandler handler, void* ctx,
                                           uint64_t* out_id);
bool event_state_unsubscribe(uint64_t id);
bool event_state_host_publish(uint32_t event_id, uint8_t availability, uint8_t value, int32_t slot);
bool event_state_has_active_declaration(uint32_t event_id);
uint32_t event_state_dispatch_pending();
void event_state_reset_for_test();

}  // namespace qol::infra
