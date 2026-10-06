#include "infra/settings_catalog.h"

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

namespace qol::infra {
namespace {
using namespace contracts;

std::mutex g_mutex;
std::array<SettingsCategoryView, kSettingsMaxCategories> g_categories{};
std::array<bool, kSettingsMaxCategories> g_category_owner_active{};
std::array<SettingsActionRecord, kSettingsMaxItems> g_items{};
uint32_t g_category_count = 0;
uint32_t g_item_count = 0;
uint64_t g_generation = 1;

struct Snapshot {
    CatalogView view{};
    std::array<SettingsCategoryView, kSettingsMaxCategories> categories{};
    std::array<SettingsItemView, kSettingsMaxItems> items{};
};
struct SnapshotSlot {
    SettingsCatalogToken token = 0;
    std::unique_ptr<Snapshot> snapshot;
};
std::array<SnapshotSlot, kSettingsMaxSnapshots> g_snapshots{};
uint64_t g_next_token = 1;

QolResult fail(int32_t code, const char* text) {
    return error(code, text, static_cast<uint32_t>(std::strlen(text)));
}

template <std::size_t N>
bool copy_text(char (&out)[N], const char* in, bool required = true) {
    if (in == nullptr) return !required;
    const std::size_t length = strnlen(in, N);
    if (length == N || (required && length == 0)) return false;
    std::memcpy(out, in, length + 1);
    return true;
}

bool equal(const SettingsCategoryView& a, const SettingsCategoryView& b) {
    return std::strcmp(a.id, b.id) == 0 && std::strcmp(a.owner, b.owner) == 0 &&
           std::strcmp(a.title, b.title) == 0 && std::strcmp(a.description, b.description) == 0;
}

bool equal(const SettingsActionRecord& a, const SettingsActionRecord& b) {
    const auto& x = a.metadata;
    const auto& y = b.metadata;
    return std::strcmp(x.id, y.id) == 0 && std::strcmp(x.category_id, y.category_id) == 0 &&
           std::strcmp(x.owner, y.owner) == 0 && std::strcmp(x.label, y.label) == 0 &&
           std::strcmp(x.description, y.description) == 0 && x.type == y.type &&
           std::strcmp(x.config_key, y.config_key) == 0 && x.owner_enable == y.owner_enable &&
           a.open == b.open && a.available == b.available && a.user == b.user;
}
}  // namespace

QolResult settings_catalog_register_category(const SettingsCategorySpec* category,
                                             const SettingsItemSpec* items, uint32_t count) {
    if (category == nullptr || category->size < sizeof(SettingsCategorySpec) || items == nullptr ||
        count == 0) return fail(kQolErrInvalidArgument, "invalid settings category bundle");
    if (count > kSettingsMaxItems) return fail(kQolErrBlocked, "settings item capacity exhausted");
    SettingsCategoryView candidate{};
    if (!copy_text(candidate.id, category->id) || !copy_text(candidate.owner, category->owner) ||
        !copy_text(candidate.title, category->title) ||
        !copy_text(candidate.description, category->description, false)) {
        return fail(kQolErrInvalidArgument, "invalid or oversized settings category text");
    }
    std::array<SettingsActionRecord, kSettingsMaxItems> pending{};
    for (uint32_t i = 0; i < count; ++i) {
        const auto& spec = items[i];
        auto& record = pending[i];
        auto& meta = record.metadata;
        if (spec.size < sizeof(SettingsItemSpec) || !copy_text(meta.id, spec.id) ||
            !copy_text(meta.category_id, spec.category_id) || !copy_text(meta.owner, spec.owner) ||
            !copy_text(meta.label, spec.label) ||
            !copy_text(meta.description, spec.description, false) ||
            std::strcmp(meta.category_id, candidate.id) != 0 ||
            std::strcmp(meta.owner, candidate.owner) != 0 ||
            (spec.owner_enable != 0 && spec.owner_enable != 1)) {
            return fail(kQolErrInvalidArgument, "invalid settings item metadata");
        }
        meta.type = spec.type;
        meta.owner_enable = spec.owner_enable;
        record.open = spec.open;
        record.available = spec.available;
        record.user = spec.user;
        if (spec.type == SettingsItemType::kToggle) {
            if (spec.open != nullptr || !copy_text(meta.config_key, spec.config_key)) {
                return fail(kQolErrInvalidArgument, "invalid settings toggle fields");
            }
        } else if (spec.type == SettingsItemType::kAction) {
            if (spec.open == nullptr || spec.config_key != nullptr || spec.owner_enable != 0) {
                return fail(kQolErrInvalidArgument, "invalid settings action fields");
            }
        } else {
            return fail(kQolErrInvalidArgument, "unknown settings item type");
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (std::strcmp(pending[j].metadata.id, meta.id) == 0) {
                return fail(kQolErrConflict, "duplicate settings item id");
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t c = 0; c < g_category_count; ++c) {
        if (std::strcmp(g_categories[c].id, candidate.id) != 0) continue;
        if (!equal(g_categories[c], candidate)) return fail(kQolErrConflict, "settings category conflict");
        uint32_t matched = 0;
        for (uint32_t i = 0; i < g_item_count; ++i) {
            if (std::strcmp(g_items[i].metadata.category_id, candidate.id) != 0) continue;
            if (matched >= count || !equal(g_items[i], pending[matched])) {
                return fail(kQolErrConflict, "settings item bundle conflict");
            }
            ++matched;
        }
        return matched == count ? ok() : fail(kQolErrConflict, "settings item bundle conflict");
    }
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = 0; j < g_item_count; ++j) {
            if (std::strcmp(pending[i].metadata.id, g_items[j].metadata.id) == 0) {
                return fail(kQolErrConflict, "settings item id conflict");
            }
        }
    }
    if (g_category_count == kSettingsMaxCategories || count > kSettingsMaxItems - g_item_count) {
        return fail(kQolErrBlocked, "settings catalog capacity exhausted");
    }
    g_categories[g_category_count++] = candidate;
    g_category_owner_active[g_category_count - 1] = true;
    for (uint32_t i = 0; i < count; ++i) g_items[g_item_count++] = pending[i];
    ++g_generation;
    return ok();
}

QolResult settings_catalog_register_category_metadata(const SettingsCategorySpec* category) {
    if (category == nullptr || category->size < sizeof(SettingsCategorySpec)) {
        return fail(kQolErrInvalidArgument, "invalid settings category metadata");
    }
    SettingsCategoryView candidate{};
    if (!copy_text(candidate.id, category->id) || !copy_text(candidate.owner, category->owner) ||
        !copy_text(candidate.title, category->title) ||
        !copy_text(candidate.description, category->description, false)) {
        return fail(kQolErrInvalidArgument, "invalid or oversized settings category text");
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < g_category_count; ++i) {
        if (std::strcmp(g_categories[i].id, candidate.id) != 0) continue;
        return equal(g_categories[i], candidate) ? ok() :
            fail(kQolErrConflict, "settings category conflict");
    }
    if (g_category_count == kSettingsMaxCategories) {
        return fail(kQolErrBlocked, "settings category capacity exhausted");
    }
    g_categories[g_category_count++] = candidate;
    g_category_owner_active[g_category_count - 1] = true;
    ++g_generation;
    return ok();
}

QolResult settings_catalog_register_items(const char* owner, const char* category_id,
                                         const SettingsItemSpec* items, uint32_t count) {
    char owner_key[kSettingsIdCapacity]{};
    char category_key[kSettingsIdCapacity]{};
    if (!copy_text(owner_key, owner) || !copy_text(category_key, category_id) || items == nullptr ||
        count == 0) return fail(kQolErrInvalidArgument, "invalid shared settings item bundle");
    if (count > kSettingsMaxItems) return fail(kQolErrBlocked, "settings item capacity exhausted");

    std::array<SettingsActionRecord, kSettingsMaxItems> pending{};
    for (uint32_t i = 0; i < count; ++i) {
        const auto& spec = items[i];
        auto& record = pending[i];
        auto& meta = record.metadata;
        if (spec.size < sizeof(SettingsItemSpec) || !copy_text(meta.id, spec.id) ||
            !copy_text(meta.category_id, spec.category_id) || !copy_text(meta.owner, spec.owner) ||
            !copy_text(meta.label, spec.label) || !copy_text(meta.description, spec.description, false) ||
            std::strcmp(meta.category_id, category_key) != 0 || std::strcmp(meta.owner, owner_key) != 0 ||
            (spec.owner_enable != 0 && spec.owner_enable != 1)) {
            return fail(kQolErrInvalidArgument, "invalid shared settings item metadata");
        }
        meta.type = spec.type;
        meta.owner_enable = spec.owner_enable;
        record.open = spec.open;
        record.available = spec.available;
        record.user = spec.user;
        if (spec.type == SettingsItemType::kToggle) {
            if (spec.open != nullptr || !copy_text(meta.config_key, spec.config_key)) {
                return fail(kQolErrInvalidArgument, "invalid settings toggle fields");
            }
        } else if (spec.type == SettingsItemType::kAction) {
            if (spec.open == nullptr || spec.config_key != nullptr || spec.owner_enable != 0) {
                return fail(kQolErrInvalidArgument, "invalid settings action fields");
            }
        } else {
            return fail(kQolErrInvalidArgument, "unknown settings item type");
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (std::strcmp(pending[j].metadata.id, meta.id) == 0) {
                return fail(kQolErrConflict, "duplicate settings item id");
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    bool category_found = false;
    for (uint32_t c = 0; c < g_category_count; ++c) {
        if (std::strcmp(g_categories[c].id, category_key) == 0) {
            category_found = true;
            break;
        }
    }
    if (!category_found) return fail(kQolErrNotFound, "settings category not found");

    uint32_t owner_items = 0;
    for (uint32_t i = 0; i < g_item_count; ++i) {
        const auto& existing = g_items[i];
        if (std::strcmp(existing.metadata.owner, owner_key) == 0 &&
            std::strcmp(existing.metadata.category_id, category_key) == 0) {
            if (owner_items >= count || !equal(existing, pending[owner_items])) {
                return fail(kQolErrConflict, "settings item bundle conflict");
            }
            ++owner_items;
            continue;
        }
        for (uint32_t j = 0; j < count; ++j) {
            if (std::strcmp(existing.metadata.id, pending[j].metadata.id) == 0) {
                return fail(kQolErrConflict, "settings item id conflict");
            }
        }
    }
    if (owner_items == count) return ok();
    if (owner_items != 0) return fail(kQolErrConflict, "settings item bundle conflict");

    if (count > kSettingsMaxItems - g_item_count) {
        return fail(kQolErrBlocked, "settings catalog item capacity exhausted");
    }
    for (uint32_t i = 0; i < count; ++i) g_items[g_item_count++] = pending[i];
    ++g_generation;
    return ok();
}

QolResult settings_catalog_unregister_owner(const char* owner) {
    char key[kSettingsIdCapacity]{};
    if (!copy_text(key, owner)) return fail(kQolErrInvalidArgument, "invalid settings owner");
    std::lock_guard<std::mutex> lock(g_mutex);
    uint32_t categories = 0;
    uint32_t items = 0;
    for (uint32_t i = 0; i < g_item_count; ++i) {
        if (std::strcmp(g_items[i].metadata.owner, key) != 0) {
            g_items[items++] = g_items[i];
        }
    }
    for (uint32_t i = 0; i < g_category_count; ++i) {
        const auto category = g_categories[i];
        bool owner_active = g_category_owner_active[i];
        if (std::strcmp(category.owner, key) == 0) owner_active = false;
        bool retain = false;
        for (uint32_t j = 0; j < items; ++j) {
            if (std::strcmp(g_items[j].metadata.category_id, category.id) == 0) {
                retain = true;
                break;
            }
        }
        if (retain || owner_active) {
            g_categories[categories] = category;
            g_category_owner_active[categories++] = owner_active;
        }
    }
    if (categories != g_category_count || items != g_item_count) ++g_generation;
    for (uint32_t i = categories; i < g_category_count; ++i) {
        g_categories[i] = {};
        g_category_owner_active[i] = false;
    }
    for (uint32_t i = items; i < g_item_count; ++i) g_items[i] = {};
    g_category_count = categories;
    g_item_count = items;
    return ok();
}

QolResult settings_catalog_snapshot_acquire(const CatalogView** out_view,
                                            SettingsCatalogToken* out_token) {
    if (out_view != nullptr) *out_view = nullptr;
    if (out_token != nullptr) *out_token = 0;
    if (out_view == nullptr || out_token == nullptr) return fail(kQolErrInvalidArgument, "null snapshot output");
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& slot : g_snapshots) {
        if (slot.snapshot != nullptr) continue;
        std::unique_ptr<Snapshot> snapshot(new (std::nothrow) Snapshot{});
        if (snapshot == nullptr) return fail(kQolErrInternal, "settings snapshot allocation failed");
        snapshot->categories = g_categories;
        for (uint32_t i = 0; i < g_item_count; ++i) snapshot->items[i] = g_items[i].metadata;
        snapshot->view = {sizeof(CatalogView), g_generation, g_category_count, g_item_count,
                          snapshot->categories.data(), snapshot->items.data()};
        slot.token = g_next_token++;
        slot.snapshot = std::move(snapshot);
        *out_view = &slot.snapshot->view;
        *out_token = slot.token;
        return ok();
    }
    return fail(kQolErrBlocked, "settings snapshot capacity exhausted");
}

QolResult settings_catalog_snapshot_release(SettingsCatalogToken token) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& slot : g_snapshots) {
        if (token != 0 && slot.token == token && slot.snapshot != nullptr) {
            slot.snapshot.reset();
            slot.token = 0;
            return ok();
        }
    }
    return fail(kQolErrNotFound, "settings snapshot token not found");
}

QolResult settings_catalog_resolve_item(const char* item_id, uint64_t generation,
                                         SettingsActionRecord* out) {
    char id[kSettingsIdCapacity]{};
    if (out == nullptr || !copy_text(id, item_id)) return fail(kQolErrInvalidArgument, "invalid settings action id");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (generation != g_generation) return fail(kQolErrConflict, "stale settings catalog generation");
    for (uint32_t i = 0; i < g_item_count; ++i) {
        if (std::strcmp(g_items[i].metadata.id, id) != 0) continue;
        *out = g_items[i];
        return ok();
    }
    return fail(kQolErrNotFound, "settings item not found");
}

QolResult settings_catalog_resolve_action(const char* item_id, uint64_t generation,
                                          SettingsActionRecord* out) {
    auto result = settings_catalog_resolve_item(item_id, generation, out);
    if (!succeeded(result)) return result;
    if (out->metadata.type != SettingsItemType::kAction) {
        return fail(kQolErrInvalidArgument, "settings item is not an action");
    }
    return ok();
}

void settings_catalog_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_categories = {};
    g_category_owner_active = {};
    g_items = {};
    g_category_count = 0;
    g_item_count = 0;
    ++g_generation;
    // Acquired views retain their lifetime even across test reset.
}
}  // namespace qol::infra
