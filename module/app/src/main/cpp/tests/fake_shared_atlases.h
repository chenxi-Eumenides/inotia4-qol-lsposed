#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <vector>

namespace qol::tests {

class FakeSharedAtlases {
public:
    struct Unit {
        bool loaded = false;
        uint32_t generation = 0;
        uint32_t physical_unloads = 0;
    };
    struct NativeConsumer {
        std::vector<std::pair<int32_t, uint32_t>> images;
    };

    void load(int32_t id) {
        auto& image = units_[id];
        // IMGSYS UnitLoad 幂等，不增加引用数或替换仍有效的贴图。
        if (!image.loaded) {
            image.loaded = true;
            ++image.generation;
        }
    }

    void unload(int32_t id) {
        auto& image = units_[id];
        ++image.physical_unloads;
        image.loaded = false;
    }

    Unit& unit(int32_t id) { return units_[id]; }

    NativeConsumer native_consumer(std::initializer_list<int32_t> ids) {
        NativeConsumer consumer;
        for (int32_t id : ids) {
            load(id);
            consumer.images.emplace_back(id, unit(id).generation);
        }
        return consumer;
    }

    bool native_draw(const NativeConsumer& consumer) const {
        for (const auto& [id, generation] : consumer.images) {
            const auto it = units_.find(id);
            if (it == units_.end() || !it->second.loaded || it->second.generation != generation) {
                return false;
            }
        }
        return true;
    }

    uint32_t protected_unloads() const {
        uint32_t count = 0;
        for (int32_t id : {0x0f, 0x59, 0x4f, 0x50}) {
            const auto it = units_.find(id);
            if (it != units_.end()) count += it->second.physical_unloads;
        }
        return count;
    }

private:
    std::map<int32_t, Unit> units_;
};

}  // namespace qol::tests
