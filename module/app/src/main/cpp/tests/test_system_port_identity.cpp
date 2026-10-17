#include "contracts/game_ports/system_port.h"
#include "contracts/game_ports/ports.h"
#include "game/adapter_ports/system_port.h"
#include "game/state/system_snapshot.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <string>
#include <thread>

namespace qol::game {

std::string build_system_snapshot_json() { return "{}"; }
std::string build_system_game_json(const std::string&) { return "{}"; }
std::string build_system_info_json(const std::string& version, const std::string& package) {
    return "{\"version\":\"" + version + "\",\"package\":\"" + package + "\"}";
}
std::string build_system_frame_json() { return "{\"frame\":0}"; }
std::string build_system_events_json() { return "{\"events\":[]}"; }

}  // namespace qol::game

int main() {
    const auto& port = qol::game::system_port();
    assert(qol::game::system_port_version() == nullptr);
    assert(qol::game::system_port_game_version() == nullptr);
    assert(qol::game::system_port_package() == nullptr);
    std::atomic<bool> start{false};
    std::atomic<bool> valid{true};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (int index = 0; index < 20000; ++index) {
            const std::string suffix = std::to_string(index % 10);
            const std::string version = "v" + suffix;
            const std::string package = "p" + suffix;
            qol::game::system_port_set_identity(version.c_str(), package.c_str(), "1.3.2");
        }
    });
    std::thread reader([&] {
        char output[128]{};
        while (!start.load(std::memory_order_acquire)) {}
        for (int index = 0; index < 20000; ++index) {
            uint32_t size = 0;
            const QolResult result = port.read.body_json(
                static_cast<int32_t>(qol::contracts::QolSystemJsonKind::kInfo), output,
                sizeof(output), &size);
            if (!qol::contracts::succeeded(result) || size == 0) {
                valid.store(false, std::memory_order_relaxed);
                continue;
            }
            const std::string body(output, size);
            bool match = false;
            for (int suffix = 0; suffix < 10; ++suffix) {
                const std::string expected = "{\"version\":\"v" + std::to_string(suffix) +
                                             "\",\"package\":\"p" +
                                             std::to_string(suffix) + "\"}";
                match = match || body == expected;
            }
            if (!match) valid.store(false, std::memory_order_relaxed);
        }
    });
    qol::game::system_port_set_identity("v0", "p0", "1.3.2");
    assert(std::strcmp(qol::game::system_port_version(), "v0") == 0);
    assert(std::strcmp(qol::game::system_port_game_version(), "1.3.2") == 0);
    assert(std::strcmp(qol::game::system_port_package(), "p0") == 0);
    start.store(true, std::memory_order_release);
    writer.join();
    reader.join();
    return valid.load(std::memory_order_relaxed) ? 0 : 1;
}
