#pragma once

#include <cstdint>

#include "result.h"

namespace qol::contracts {

constexpr uint16_t kSettingsCatalogMajor = 1;
constexpr uint16_t kSettingsCatalogMinor = 3;
constexpr uint32_t kSettingsMaxCategories = 32;
constexpr uint32_t kSettingsMaxItems = 128;
constexpr uint32_t kSettingsMaxSnapshots = 8;
// 64-bit implementation budgets: resident records ~162 KiB; each lazily allocated
// metadata snapshot ~159 KiB, at most ~1.25 MiB for 8 tokens. Not a UI row budget.
// UTF-8 byte capacities include the terminating NUL; oversize text is rejected, not truncated.
constexpr uint32_t kSettingsIdCapacity = 96;
constexpr uint32_t kSettingsTitleCapacity = 128;
constexpr uint32_t kSettingsDescriptionCapacity = 512;
constexpr uint32_t kSettingsConfigKeyCapacity = 128;
constexpr const char* kSettingsBasicCategoryId = "basic";
constexpr const char* kSettingsOptimizationCategoryId = "optimization";

enum class SettingsItemType : uint32_t { kToggle = 0, kAction = 1 };

// Both callbacks run only on the game main thread. user and callback code must remain
// valid for the process lifetime (static built-ins); registration never calls them.
using SettingsOpenFn = QolResult (*)(void* user);
using SettingsAvailableFn = QolResult (*)(void* user, int32_t* out_available,
                                         char* reason, uint32_t capacity);

struct SettingsCategorySpec {
    uint32_t size;
    const char* id;
    const char* owner;
    const char* title;
    const char* description;  // nullptr is normalized to empty text
};

struct SettingsItemSpec {
    uint32_t size;
    const char* id;
    const char* category_id;
    const char* owner;
    const char* label;
    const char* description;
    SettingsItemType type;
    const char* config_key;  // required for toggle; nullptr for action
    int32_t owner_enable;   // 0/1; toggle remains editable when its owner gate is disabled;
                            // never requests lifecycle activation
    SettingsOpenFn open;    // required for action; nullptr for toggle
    SettingsAvailableFn available;  // optional; success + out_available=0 rejects
    void* user;
};

// Snapshot metadata owns all text and contains no executable pointers or user context.
struct SettingsCategoryView {
    char id[kSettingsIdCapacity];
    char owner[kSettingsIdCapacity];
    char title[kSettingsTitleCapacity];
    char description[kSettingsDescriptionCapacity];
};

struct SettingsItemView {
    char id[kSettingsIdCapacity];
    char category_id[kSettingsIdCapacity];
    char owner[kSettingsIdCapacity];
    char label[kSettingsTitleCapacity];
    char description[kSettingsDescriptionCapacity];
    SettingsItemType type;
    char config_key[kSettingsConfigKeyCapacity];
    int32_t owner_enable;
};

struct CatalogView {
    uint32_t size;
    uint64_t generation;  // nonzero; changes only on actual registration/removal
    uint32_t category_count;
    uint32_t item_count;
    const SettingsCategoryView* categories;
    const SettingsItemView* items;
};
using SettingsCatalogToken = uint64_t;

enum class SettingsOwnerState : uint32_t {
    kUnknown = 0,
    kRegistered = 1,
    kResolving = 2,
    kBlocked = 3,
    kUnsupported = 4,
    kInvalid = 5,
    kInstalledDisabled = 6,
    kEnabled = 7,
    kDeactivating = 8,
    kInactive = 9,
    kFailed = 10,
};

struct SettingsItemAvailability {
    uint32_t size;
    int32_t operable;
    uint32_t owner_state;
    char reason[kSettingsDescriptionCapacity];
};

struct QolSettingsCatalogApi {
    uint32_t size;
    uint16_t major;
    uint16_t minor;
    // Atomic category + complete, ordered, nonempty item bundle. Identical owner/content is
    // idempotent; any differing content or id ownership collision is a conflict.
    QolResult (*register_category)(const SettingsCategorySpec* category,
                                   const SettingsItemSpec* items, uint32_t count);
    // Explicit removal only; independent of panel provider removal or owner disable.
    QolResult (*unregister_owner)(const char* owner);
    // Thread-safe, at most 8 live tokens; view remains valid across removal until release.
    QolResult (*snapshot_acquire)(const CatalogView** out_view, SettingsCatalogToken* out_token);
    QolResult (*snapshot_release)(SettingsCatalogToken token);
    // Main-thread only, resolves current id and exact generation; requires enabled owner.
    QolResult (*invoke_action)(const char* item_id, uint64_t generation);
    // Main-thread only; re-resolves the live item and probes optional availability without
    // opening it or changing configuration/lifecycle state.
    QolResult (*query_item_availability)(const char* item_id, uint64_t generation,
                                         SettingsItemAvailability* out);
    // Atomic append of items owned by owner into an existing category. Category metadata is
    // immutable; the caller must be a registered static plugin owner.
    QolResult (*register_items)(const char* owner, const char* category_id,
                                const SettingsItemSpec* items, uint32_t count);
    // Registers immutable category metadata without creating a synthetic item.
    QolResult (*register_category_metadata)(const SettingsCategorySpec* category);
};

}  // namespace qol::contracts
