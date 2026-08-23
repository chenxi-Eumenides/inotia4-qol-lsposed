#pragma once

#include <array>
#include <cstdint>

namespace virtual_bag {

constexpr int kBagCount = 5;
constexpr int kMaxCapacity = 16;
constexpr int kSlotCount = 16;
constexpr std::array<uint8_t, kBagCount> kFixedCapacities = {16, 8, 4, 0, 0};

enum class BagType : uint8_t {
    kNone = 0,
    kHandbag = 1,
    kSmallBackpack = 2,
    kMediumBackpack = 3,
    kLargeBackpack = 4,
};

enum class Mode {
    kOriginal,
    kModule,
    kExitingModule,
};

struct Item {
    int category = 0;
    int count = 0;
};

struct State {
    std::array<uint8_t, kBagCount> types{};
    std::array<uint8_t, kBagCount> capacities{};
    std::array<std::array<Item, kSlotCount>, kBagCount> items{};
    Mode mode = Mode::kOriginal;
    int original_selected = 0;
    int selected = -1;
    int inspected = -1;
};

enum class ClickResult {
    kIgnored,
    kSelected,
    kInspected,
};

inline bool valid_index(int index) {
    return index >= 0 && index < kBagCount;
}

inline bool valid_capacity(int capacity) {
    return capacity >= 0 && capacity <= kMaxCapacity;
}

inline bool valid_type(int type) {
    return type >= static_cast<int>(BagType::kNone) &&
           type <= static_cast<int>(BagType::kLargeBackpack);
}

inline void normalize(State* state) {
    if (state == nullptr) return;
    for (int index = 0; index < kBagCount; ++index) {
        if (!valid_type(state->types[index])) state->types[index] = 0;
        state->capacities[index] = kFixedCapacities[index];
    }
    for (auto& bag : state->items) {
        for (Item& item : bag) {
            if (item.category < 0 || item.count < 0) {
                item = {};
            }
        }
    }
    if (state->original_selected < 0 || state->original_selected >= 6) {
        state->original_selected = 0;
    }
    if (!valid_index(state->selected)) {
        state->selected = -1;
    }
    if (state->mode == Mode::kModule &&
        (!valid_index(state->selected) || state->capacities[state->selected] == 0)) {
        state->mode = Mode::kOriginal;
        state->selected = -1;
    }
    if (state->inspected != state->selected) state->inspected = -1;
}

inline void enter_original(State* state, int bag) {
    if (state == nullptr) return;
    state->mode = Mode::kOriginal;
    state->original_selected = bag >= 0 && bag < 6 ? bag : 0;
    state->selected = -1;
    state->inspected = -1;
}

inline void begin_exit_module(State* state) {
    if (state == nullptr) return;
    state->mode = Mode::kExitingModule;
    state->inspected = -1;
}

inline bool set_test_equipped(State* state, int index, int bag_type) {
    if (state == nullptr || !valid_index(index) || !valid_type(bag_type) || bag_type == 0) {
        return false;
    }
    state->types[index] = static_cast<uint8_t>(bag_type);
    normalize(state);
    return true;
}

inline ClickResult click(State* state, int index) {
    if (state == nullptr || !valid_index(index)) {
        return ClickResult::kIgnored;
    }
    if (kFixedCapacities[index] == 0) {
        return ClickResult::kIgnored;
    }
    if (state->selected == index) {
        state->mode = Mode::kModule;
        state->inspected = index;
        return ClickResult::kInspected;
    }
    state->mode = Mode::kModule;
    state->selected = index;
    state->inspected = -1;
    return ClickResult::kSelected;
}

inline bool set_item(State* state, int bag, int slot, int category, int count) {
    if (state == nullptr || !valid_index(bag) || slot < 0 || slot >= kSlotCount ||
        category < 0 || count < 0) {
        return false;
    }
    state->items[bag][slot] = Item{category, count};
    return true;
}

}  // namespace virtual_bag
