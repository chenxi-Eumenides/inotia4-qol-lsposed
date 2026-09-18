#include <cassert>
#include <cstdint>

#include "plugins/attribute_range_display/feature.h"

namespace {

int g_add_calls = 0;
int g_make_calls = 0;

void original_add(void*, int, int, int) { ++g_add_calls; }

uintptr_t original_make(void* item, void*, void*) {
    ++g_make_calls;
    return reinterpret_cast<uintptr_t>(item) + 1;
}

}  // namespace

int main() {
    qol::contracts::PluginContext context{};
    qol::features::attribute_range_display::set_context(context);
    qol::features::attribute_range_display::on_resource_ready(
        context, qol::features::attribute_range_display::kResourceIdAddOption,
        reinterpret_cast<void*>(&original_add));
    qol::features::attribute_range_display::on_resource_ready(
        context, qol::features::attribute_range_display::kResourceIdMakeItem,
        reinterpret_cast<void*>(&original_make));
    qol::features::attribute_range_display::set_active(false);

    char text[] = "value";
    char* builder_text = text;
    qol::features::attribute_range_display::uid_desc_add_option_wrapper(&builder_text, 0, 1, 3);
    assert(g_add_calls == 1);
    assert(builder_text == text);

    void* item = reinterpret_cast<void*>(0x1000);
    assert(qol::features::attribute_range_display::uid_desc_make_item_wrapper(
               item, nullptr, nullptr) == 0x1001);
    assert(g_make_calls == 1);
    return 0;
}
