#include "bag_source_registry.h"

namespace qol::features::core_item::bags {

BagSourceRegistry::BagSourceRegistry(MainThreadPredicate is_main_thread)
    : is_main_thread_(is_main_thread), sources_(), generation_(0) {}

bool BagSourceRegistry::on_main_thread() const {
    return is_main_thread_ != nullptr && is_main_thread_();
}

bool BagSourceRegistry::register_source(const contracts::QolBagSource* source) {
    if (!on_main_thread() || !contracts::qol_bag_source_valid(source)) return false;
    if (source->space_id < contracts::kQolBagSpaceFirstRegistered) return false;
    if (find(source->space_id) != nullptr) return false;
    sources_.push_back(source);
    ++generation_;
    return true;
}

bool BagSourceRegistry::unregister_source(int32_t space_id) {
    if (!on_main_thread() || space_id < contracts::kQolBagSpaceFirstRegistered) return false;
    for (std::size_t index = 0; index < sources_.size(); ++index) {
        if (sources_[index] == nullptr || sources_[index]->space_id != space_id) continue;
        for (std::size_t move = index + 1; move < sources_.size(); ++move) {
            sources_[move - 1] = sources_[move];
        }
        sources_.pop_back();
        ++generation_;
        return true;
    }
    return false;
}

const contracts::QolBagSource* BagSourceRegistry::find(int32_t space_id) const {
    if (!on_main_thread()) return nullptr;
    for (const contracts::QolBagSource* source : sources_) {
        if (source != nullptr && source->space_id == space_id) return source;
    }
    return nullptr;
}

uint64_t BagSourceRegistry::generation() const {
    return generation_;
}

void BagSourceRegistry::clear() {
    if (!on_main_thread() || sources_.empty()) return;
    sources_.clear();
    ++generation_;
}

}  // namespace qol::features::core_item::bags
