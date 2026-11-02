#include "game/adapter_ports/character_extras_port.h"

int main() {
    return qol::game::character_extras_port().abi_version.major;
}
