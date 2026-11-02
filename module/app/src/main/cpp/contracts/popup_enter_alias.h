#pragma once

#include <cstddef>
#include <cstdint>

constexpr uintptr_t kPopupEnterAliasNone = 0;
constexpr size_t kPopupStateEntryEnterOffset = 0x10;
constexpr size_t kPopupStateEntryF3Offset = 0x28;
constexpr size_t kPopupStateEntryEventOffset = 0x38;

void popup_enter_alias_register(uintptr_t wrapper, uintptr_t original_vma);
bool popup_enter_alias_unregister(uintptr_t wrapper);
uintptr_t popup_enter_alias_resolve(uintptr_t enter);
void popup_enter_alias_clear();
