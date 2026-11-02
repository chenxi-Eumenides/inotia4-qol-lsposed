#include "contracts/game_host.h"

int main() {
    const auto* host = static_cast<const qol::contracts::QolGameHost*>(nullptr);
    return host->read_only.character.party_index(nullptr, nullptr).code;
}
