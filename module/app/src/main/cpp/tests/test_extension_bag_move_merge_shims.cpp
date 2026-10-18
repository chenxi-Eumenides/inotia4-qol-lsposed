#include <cassert>
#include <atomic>
#include <cstdint>
#include "contracts/host_sdk.h"

#include "contracts/move_merge.h"
#include "contracts/result.h"
#include "plugins/extension_bag/game_ui_virtbag.h"
#include "plugins/extension_bag/assembly/move_merge.h"

bool extension_bag_private_item_for_handle(qol::contracts::QolItemHandle, void** out_item);
bool virtual_bag_identify_native_item(void*, int* out_bag, int* out_slot);

namespace qol::features::extension_bag {

std::atomic<bool> g_provider_active{false};
bool fake_private_handle_result = false;
bool fake_identity_result = false;
int fake_drop_result = 0;
int fake_drop_calls = 0;

bool inventory_extension_provider_active() {
    return g_provider_active.load(std::memory_order_acquire);
}

}  // namespace qol::features::extension_bag

bool extension_bag_private_item_for_handle(qol::contracts::QolItemHandle, void** out_item) {
    if (qol::features::extension_bag::fake_private_handle_result) {
        *out_item = reinterpret_cast<void*>(0x1);
    }
    return qol::features::extension_bag::fake_private_handle_result;
}

bool virtual_bag_identify_native_item(void*, int* out_bag, int* out_slot) {
    if (qol::features::extension_bag::fake_identity_result) {
        *out_bag = 7;
        *out_slot = 3;
    }
    return qol::features::extension_bag::fake_identity_result;
}

VirtualBagProjectionDropResult virtual_bag_projection_drop_to_slot(void*, void*) {
    ++qol::features::extension_bag::fake_drop_calls;
    return static_cast<VirtualBagProjectionDropResult>(
        qol::features::extension_bag::fake_drop_result);
}

int main() {
    using namespace qol::contracts;
    using namespace qol::features::extension_bag;
    int32_t bag = 91;
    int32_t slot = 92;
    const QolItemHandle valid_handle{4, 3};

    g_provider_active.store(false);
    auto result = merge_projection_identify_extension_item(valid_handle, &bag, &slot);
    assert(result.code == kQolErrNotReady && bag == 91 && slot == 92);
    assert(merge_projection_drop(nullptr, nullptr) == MoveMergeProjectionDropResult::kNotHandled);
    assert(fake_drop_calls == 0);

    g_provider_active.store(true);
    fake_private_handle_result = false;
    result = merge_projection_identify_extension_item(valid_handle, &bag, &slot);
    assert(result.code == kQolErrNotFound && bag == 91 && slot == 92);

    fake_private_handle_result = true;
    fake_identity_result = false;
    result = merge_projection_identify_extension_item(valid_handle, &bag, &slot);
    assert(result.code == kQolErrNotFound && bag == 91 && slot == 92);

    fake_identity_result = true;
    result = merge_projection_identify_extension_item(valid_handle, &bag, &slot);
    assert(succeeded(result) && bag == 7 && slot == 3);

    fake_drop_result = 0;
    assert(merge_projection_drop(nullptr, nullptr) == MoveMergeProjectionDropResult::kNotHandled);
    fake_drop_result = 1;
    assert(merge_projection_drop(nullptr, nullptr) == MoveMergeProjectionDropResult::kHandled);
    fake_drop_result = 2;
    assert(merge_projection_drop(nullptr, nullptr) == MoveMergeProjectionDropResult::kRejected);
    assert(fake_drop_calls == 3);
    return 0;
}
