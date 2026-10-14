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
    QOL_PORT_EXTENSION_BAG_WRITE = 5,
    QOL_PORT_CHOICE = 6,
    QOL_PORT_SAVE = 7,
    QOL_PORT_SAVE_CALL = 8,
    QOL_PORT_CRAFT = 9,
    QOL_PORT_CRAFT_UI_WRITE = 10,
    QOL_PORT_CUSTOM_RECIPE_WRITE = 11,
    QOL_PORT_CHARACTER_EXTRAS = 12,
    QOL_PORT_INVENTORY_EXTRAS = 13,
    QOL_PORT_ITEM_EXTRAS = 14,
    QOL_PORT_SYSTEM = 15,
    QOL_PORT_CHARACTER_HTTP = 16,
    QOL_PORT_SAVE_WRITE = 17,
    QOL_PORT_WORLD_WRITE = 18,
    QOL_PORT_UI_WRITE = 19,
    QOL_PORT_CHOICE_WRITE = 20,
    QOL_PORT_UI_STATE_READ = 22,
    QOL_PORT_ITEM_MONEY_WRITE = 23,
    QOL_PORT_ITEM_DICE_WRITE = 24,
    QOL_PORT_ITEM_INVENTORY_STATE_WRITE = 25,
    QOL_PORT_CHARACTER_STATE = 26,
    QOL_PORT_CHARACTER_WRITE_OPS = 27,
    QOL_PORT_MOVE_MERGE = 28,
    QOL_PORT_SAVE_TRANSACTION = 29,
};

constexpr QolVersion kQolPortQuestVersion{1, 2};
constexpr QolVersion kQolPortWorldVersion{2, 0};
constexpr QolVersion kQolPortItemVersion{3, 3};
constexpr QolVersion kQolPortCharacterVersion{1, 0};
constexpr QolVersion kQolPortExtensionBagWriteVersion{1, 0};
constexpr QolVersion kQolPortChoiceVersion{1, 0};
constexpr QolVersion kQolPortSaveVersion{1, 0};
constexpr QolVersion kQolPortSaveCallVersion{1, 0};
constexpr QolVersion kQolPortCraftVersion{1, 1};
constexpr QolVersion kQolPortCraftUiWriteVersion{1, 1};
constexpr QolVersion kQolPortCustomRecipeWriteVersion{1, 2};
constexpr QolVersion kQolPortCharacterExtrasVersion{1, 1};
constexpr QolVersion kQolPortInventoryExtrasVersion{1, 0};
constexpr QolVersion kQolPortItemExtrasVersion{1, 0};
constexpr QolVersion kQolPortSystemVersion{1, 1};
constexpr QolVersion kQolPortCharacterHttpVersion{1, 0};
constexpr QolVersion kQolPortSaveWriteVersion{1, 0};
constexpr QolVersion kQolPortWorldWriteVersion{1, 0};
constexpr QolVersion kQolPortUiWriteVersion{1, 1};
constexpr QolVersion kQolPortUiStateReadVersion{1, 0};
constexpr QolVersion kQolPortChoiceWriteVersion{1, 0};
constexpr QolVersion kQolPortCharacterStateVersion{1, 0};
constexpr QolVersion kQolPortCharacterWriteOpsVersion{1, 0};
constexpr QolVersion kQolPortSaveTransactionVersion{1, 0};

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
        case QolPortId::QOL_PORT_EXTENSION_BAG_WRITE:
            return "QOL_PORT_EXTENSION_BAG_WRITE";
        case QolPortId::QOL_PORT_CHOICE:
            return "QOL_PORT_CHOICE";
        case QolPortId::QOL_PORT_SAVE:
            return "QOL_PORT_SAVE";
        case QolPortId::QOL_PORT_SAVE_CALL:
            return "QOL_PORT_SAVE_CALL";
        case QolPortId::QOL_PORT_CRAFT:
            return "QOL_PORT_CRAFT";
        case QolPortId::QOL_PORT_CRAFT_UI_WRITE:
            return "QOL_PORT_CRAFT_UI_WRITE";
        case QolPortId::QOL_PORT_CUSTOM_RECIPE_WRITE:
            return "QOL_PORT_CUSTOM_RECIPE_WRITE";
        case QolPortId::QOL_PORT_CHARACTER_EXTRAS:
            return "QOL_PORT_CHARACTER_EXTRAS";
        case QolPortId::QOL_PORT_INVENTORY_EXTRAS:
            return "QOL_PORT_INVENTORY_EXTRAS";
        case QolPortId::QOL_PORT_ITEM_EXTRAS:
            return "QOL_PORT_ITEM_EXTRAS";
        case QolPortId::QOL_PORT_SYSTEM:
            return "QOL_PORT_SYSTEM";
        case QolPortId::QOL_PORT_CHARACTER_HTTP:
            return "QOL_PORT_CHARACTER_HTTP";
        case QolPortId::QOL_PORT_SAVE_WRITE:
            return "QOL_PORT_SAVE_WRITE";
        case QolPortId::QOL_PORT_WORLD_WRITE:
            return "QOL_PORT_WORLD_WRITE";
        case QolPortId::QOL_PORT_UI_WRITE:
            return "QOL_PORT_UI_WRITE";
        case QolPortId::QOL_PORT_CHOICE_WRITE:
            return "QOL_PORT_CHOICE_WRITE";
        case QolPortId::QOL_PORT_UI_STATE_READ:
            return "QOL_PORT_UI_STATE_READ";
        case QolPortId::QOL_PORT_ITEM_MONEY_WRITE:
            return "QOL_PORT_ITEM_MONEY_WRITE";
        case QolPortId::QOL_PORT_ITEM_DICE_WRITE:
            return "QOL_PORT_ITEM_DICE_WRITE";
        case QolPortId::QOL_PORT_ITEM_INVENTORY_STATE_WRITE:
            return "QOL_PORT_ITEM_INVENTORY_STATE_WRITE";
        case QolPortId::QOL_PORT_CHARACTER_STATE:
            return "QOL_PORT_CHARACTER_STATE";
        case QolPortId::QOL_PORT_CHARACTER_WRITE_OPS:
            return "QOL_PORT_CHARACTER_WRITE_OPS";
        case QolPortId::QOL_PORT_MOVE_MERGE:
            return "QOL_PORT_MOVE_MERGE";
        case QolPortId::QOL_PORT_SAVE_TRANSACTION:
            return "QOL_PORT_SAVE_TRANSACTION";
    }
    return "QOL_PORT_UNKNOWN";
}

}  // namespace qol::contracts
