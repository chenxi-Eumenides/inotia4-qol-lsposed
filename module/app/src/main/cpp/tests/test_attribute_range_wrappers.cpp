#include <cassert>
#include <cstdint>

#include "game/access/attr_range_ui.h"
#include "plugins/attribute_range/feature.h"

namespace {

int g_math_calls = 0;
int g_add_calls = 0;
int g_make_calls = 0;
int g_insert_calls = 0;

int original_math(int min, int max) {
    ++g_math_calls;
    return min + max;
}

void original_add(void*, int, int, int) {
    ++g_add_calls;
}

uintptr_t original_make(void* item, void*, void*) {
    ++g_make_calls;
    return reinterpret_cast<uintptr_t>(item) + 1;
}

}  // namespace

namespace qol::game {

int attr_item_category(const void*) { return 0; }
bool attr_item_uses_fixed_option_table(int) { return false; }
bool attr_fixed_option_percent_pair(int, int, int*, int*) { return false; }
int attr_ability_level(const void*) { return 0; }
bool attr_is_jewel_category(int) { return false; }
bool attr_probe_option_range(int, int, int, void*, int*, int*) { return false; }
bool attr_probe_jewel_range(int, void*, int*, int*) { return false; }
void attr_set_math_backup(void*) {}
void attr_set_add_option_backup(void*) {}
void attr_set_make_item_backup(void*) {}
bool attr_math_ready() { return true; }
void* attr_backup_add_option() { return reinterpret_cast<void*>(&original_add); }
void* attr_backup_make_item() { return reinterpret_cast<void*>(&original_make); }
int attr_math_get_random(int min, int max) {
    return original_math(min, max);
}
bool attr_insert_value_color(char*, char*, char) {
    ++g_insert_calls;
    return true;
}

}  // namespace qol::game

int main() {
    qol::features::attribute_range::set_active(false);

    assert(qol::features::attribute_range::math_get_random_wrapper(2, 5) == 7);
    assert(g_math_calls == 1);

    char text[] = "value";
    char* builder_text = text;
    qol::features::attribute_range::uid_desc_add_option_wrapper(&builder_text, 0, 1, 3);
    assert(g_add_calls == 1);
    assert(g_insert_calls == 0);
    assert(builder_text == text);

    void* item = reinterpret_cast<void*>(0x1000);
    assert(qol::features::attribute_range::uid_desc_make_item_wrapper(item, nullptr, nullptr) ==
           0x1001);
    assert(g_make_calls == 1);
    assert(g_insert_calls == 0);
    return 0;
}
