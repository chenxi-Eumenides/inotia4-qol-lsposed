#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "result.h"
#include "version.h"

// 角色属性只读契约（capability id=`character.attributes`，v1.0）。
//
// 定位：由官方 provider `core.character` 发布，消费者按角色引用（队伍 0..2 / 佣兵槽 0..20）
// 读取主属性、attr 0..114 全量槽位与面板派生值。契约只含 POD 与函数指针；不暴露游戏指针、
// 偏移或线程细节。所有读取语义由实现决定（主线程、失败行/槽位标记）。
//
// 线程：`present`/`attr`/`main_stat`/`snapshot`/`derived` 只允许游戏主线程调用；实现返回
// Blocked 时调用方不得重试等待。

namespace qol::contracts {

constexpr const char* kCharacterAttributesCapabilityId = "character.attributes";
constexpr QolVersion kCharacterAttributesVersion{1, 0};
// attr 槽位数（id 0..114）。
constexpr int32_t kQolCharacterAttrSlotCount = 115;
constexpr int32_t kQolCharacterMainStatCount = 5;

enum QolCharacterRefKind : int32_t {
    kQolCharacterRefParty = 0,      // 队伍槽 0..2（PARTY_GetMember）
    kQolCharacterRefMercenary = 1,  // 佣兵槽 0..20（FindAsMercenarySlot / 存档回退）
};

struct QolCharacterRef {
    int32_t kind;
    int32_t index;
};

enum QolCharacterDerivedKind : int32_t {
    kQolCharacterDerivedNormalDamage = 0,
    kQolCharacterDerivedNormalMagicDamage = 1,
    kQolCharacterDerivedPhysicalDefense = 2,
    kQolCharacterDerivedHitRate1000 = 3,
    kQolCharacterDerivedEvasionRate1000 = 4,
    kQolCharacterDerivedDisplayDamageReduceRate = 5,
};

struct QolCharacterAttributeSnapshot {
    uint32_t struct_size;
    QolCharacterRef ref;
    uint8_t present;
    uint8_t reserved[3];
    int32_t main_stats[kQolCharacterMainStatCount];
    int32_t attrs[kQolCharacterAttrSlotCount];
    uint8_t attr_valid[kQolCharacterAttrSlotCount];
};

struct CharacterAttributesApi {
    uint32_t struct_size;
    // 只查活 CHAR，不物化（不读存档）。
    QolResult (*present)(QolCharacterRef ref, int32_t* out_present);
    QolResult (*attr)(QolCharacterRef ref, int32_t attr_id, int32_t* out_value);
    QolResult (*main_stat)(QolCharacterRef ref, int32_t index, int32_t* out_value);
    QolResult (*snapshot)(QolCharacterRef ref, QolCharacterAttributeSnapshot* out);
    QolResult (*derived)(QolCharacterRef ref, int32_t kind, int32_t* out_value);
};

static_assert(std::is_standard_layout<QolCharacterRef>::value &&
                  std::is_trivially_copyable<QolCharacterRef>::value,
              "QolCharacterRef must remain POD");
static_assert(std::is_standard_layout<QolCharacterAttributeSnapshot>::value &&
                  std::is_trivially_copyable<QolCharacterAttributeSnapshot>::value,
              "QolCharacterAttributeSnapshot must remain POD");
static_assert(std::is_standard_layout<CharacterAttributesApi>::value &&
                  std::is_trivially_copyable<CharacterAttributesApi>::value,
              "CharacterAttributesApi must remain POD");
static_assert(kQolCharacterAttrSlotCount == 115, "attr slot count must cover id 0..114");
static_assert(kQolCharacterMainStatCount == 5, "main stat count must be 5");
static_assert(sizeof(QolCharacterRef) == 8, "QolCharacterRef ABI size changed");
static_assert(offsetof(QolCharacterAttributeSnapshot, present) ==
                  sizeof(uint32_t) + sizeof(QolCharacterRef),
              "snapshot present offset changed");
static_assert(sizeof(QolCharacterAttributeSnapshot) == 612,
              "QolCharacterAttributeSnapshot ABI size changed");

}  // namespace qol::contracts
