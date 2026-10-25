#pragma once

#include <cstdint>
#include <vector>

#include "bag_source.h"

namespace qol::features::core_item::bags {

using MainThreadPredicate = bool (*)();

class BagSourceRegistry {
public:
    explicit BagSourceRegistry(MainThreadPredicate is_main_thread);

    bool register_source(const contracts::QolBagSource* source);
    bool unregister_source(int32_t space_id);
    const contracts::QolBagSource* find(int32_t space_id) const;
    uint64_t generation() const;
    void clear();

private:
    bool on_main_thread() const;

    MainThreadPredicate is_main_thread_;
    std::vector<const contracts::QolBagSource*> sources_;
    uint64_t generation_;
};

}  // namespace qol::features::core_item::bags
