#pragma once

#include <cstdint>

uint64_t virtual_bag_observe_item_proc_pre(void* control, uint64_t event, void* x2,
                                           void* param);
void virtual_bag_observe_item_proc_post(uint64_t observation_token, void* control,
                                        uint64_t event, void* x2, void* param,
                                        uint64_t result);
int virtual_bag_inventory_state_id();
