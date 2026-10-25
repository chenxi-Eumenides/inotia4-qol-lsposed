#pragma once

#include <cstddef>
#include <cstdint>

#include "contracts/game_ports/item_port.h"

namespace qol::features::core_item::usecases::transfer_journal {

struct JournalId {
    uint8_t bytes[16];
};

bool begin(const contracts::QolBagRef& source, int32_t source_slot,
           const contracts::QolBagRef& target, int32_t target_slot, int32_t category,
           int32_t count, JournalId* out_id);
void mark_target_written(const JournalId& id);
void mark_target_written(const JournalId& id, uint64_t payload_hash);
void clear(const JournalId& id);
std::size_t count();
bool reconcile();

}  // namespace qol::features::core_item::usecases::transfer_journal
