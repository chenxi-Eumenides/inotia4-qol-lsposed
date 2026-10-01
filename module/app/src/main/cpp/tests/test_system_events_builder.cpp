#include "game/state/system_snapshot.h"

#include <cstdio>
#include <string>

int main() {
    qol::game::SystemEventsSnapshot previous{};
    qol::game::SystemEventsSnapshot current{};
    bool has_previous = false;
    current.money = current.inv_count = current.x = current.y = 1;
    for (int i = 0; i < 3; ++i) {
        current.hp[i] = current.mp[i] = current.level[i] = 1;
        current.exp[i] = 1;
    }
    if (qol::game::build_events_json(current, previous, has_previous) != "{\"events\":[]}" ||
        !has_previous) return 1;

    previous = current;
    current.money = 20;
    current.inv_count = 4;
    current.x = 2;
    current.hp[0] = 11;
    current.mp[0] = 12;
    current.level[0] = 13;
    current.exp[0] = 14;
    const std::string expected =
        "{\"events\":[{\"type\":\"money\",\"old\":1,\"new\":20},"
        "{\"type\":\"inventory\",\"old\":1,\"new\":4},"
        "{\"type\":\"move\",\"old\":0,\"new\":0},"
        "{\"type\":\"hp\",\"role\":0,\"old\":1,\"new\":11},"
        "{\"type\":\"mp\",\"role\":0,\"old\":1,\"new\":12},"
        "{\"type\":\"level_up\",\"role\":0,\"old\":1,\"new\":13},"
        "{\"type\":\"exp\",\"role\":0,\"old\":1,\"new\":14}]}";
    if (qol::game::build_events_json(current, previous, has_previous) != expected) return 2;
    if (qol::game::build_events_json(current, previous, has_previous) != "{\"events\":[]}") return 3;

    qol::game::SystemEventsSnapshot unavailable = current;
    unavailable.money = -1;
    unavailable.hp[0] = -1;
    unavailable.x = -1;
    const std::string unavailable_json =
        qol::game::build_events_json(unavailable, previous, has_previous);
    if (unavailable_json != "{\"events\":[]}") return 4;
    if (previous.money != -1 || previous.hp[0] != -1 || previous.x != -1) return 5;

    std::puts("system_events_builder: golden fields and diff semantics passed");
    return 0;
}
