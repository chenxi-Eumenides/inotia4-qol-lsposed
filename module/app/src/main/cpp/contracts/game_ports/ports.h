#pragma once

// Game Adapter Port 契约：版本耦合的低层端口通过插件描述符声明并由宿主注入。

#include <cstdint>

#include "../version.h"

namespace qol::contracts {

enum class QolPortId : uint32_t {
    QOL_PORT_QUEST = 1,
    QOL_PORT_WORLD = 2,
    QOL_PORT_ITEM = 3,
    QOL_PORT_CHARACTER = 4,
};

constexpr QolVersion kQolPortQuestVersion{1, 0};
constexpr QolVersion kQolPortWorldVersion{1, 0};
constexpr QolVersion kQolPortItemVersion{1, 0};
constexpr QolVersion kQolPortCharacterVersion{1, 0};

struct QolPortRequirement {
    QolPortId id;
    QolVersionRange range;
};

constexpr const char* port_id_name(QolPortId id) {
    switch (id) {
        case QolPortId::QOL_PORT_QUEST:
            return "QOL_PORT_QUEST";
        case QolPortId::QOL_PORT_WORLD:
            return "QOL_PORT_WORLD";
        case QolPortId::QOL_PORT_ITEM:
            return "QOL_PORT_ITEM";
        case QolPortId::QOL_PORT_CHARACTER:
            return "QOL_PORT_CHARACTER";
    }
    return "QOL_PORT_UNKNOWN";
}

}  // namespace qol::contracts
