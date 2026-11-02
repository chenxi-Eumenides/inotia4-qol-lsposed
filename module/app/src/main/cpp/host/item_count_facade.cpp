#include "host/item_count_facade.h"

#include "game/adapter_ports/item_count_facade.h"

namespace qol::host {

void host_item_count_set_extended_mode(qol::game::ItemCountExtendedMode mode) {
    qol::game::item_count_set_extended_mode(mode);
}

const contracts::QolItemCountApi& host_item_count_api() {
    return qol::game::item_count_api();
}

const contracts::QolItemCountApi* host_item_count_api_if_resident() {
    return qol::game::item_count_api_if_resident();
}

}  // namespace qol::host
