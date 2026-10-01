#include "contracts/game_ports/character_http_port.h"
#include "game/adapter_ports/character_http_port.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

bool g_main_thread = false;
int g_pass = 0;
int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (false)

}  // namespace

namespace qol::game {
bool game_host_is_main_thread() { return g_main_thread; }
std::string build_character_party_json() { return "[]"; }
std::string build_character_skills_json() { return "[]"; }
std::string build_character_mercenaries_json() { return "[]"; }
}

std::string data_player_json() { return "{\"player\":true}"; }

int main() {
    const auto& port = qol::game::character_http_port();
    char output[64]{};
    uint32_t output_size = 99;
    const QolResult wrong_thread = port.read.body_json(
        static_cast<int32_t>(qol::contracts::QolCharacterHttpJsonKind::kParty), output,
        sizeof(output), &output_size);
    CHECK(wrong_thread.code == kQolErrBlocked);
    CHECK(output_size == 0);

    g_main_thread = true;
    output_size = 0;
    const QolResult main_thread = port.read.body_json(
        static_cast<int32_t>(qol::contracts::QolCharacterHttpJsonKind::kParty), output,
        sizeof(output), &output_size);
    CHECK(qol::contracts::succeeded(main_thread));
    CHECK(std::string(output, output_size) == "[]");
    std::printf("character_http_port: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
