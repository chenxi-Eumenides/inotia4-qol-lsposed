#include "plugins/extension_bag/model/bag_catalog.h"

#include <cstdio>

void extension_bag_test_set_active_count(int count);

int main() {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* message) {
        if (!condition) {
            ++failures;
            std::printf("FAIL %s\n", message);
        }
    };

    check(virtual_bag::kMaxExtensionBags == 8, "maximum bag count");
    check(virtual_bag::kSlotCount == 16, "slot count");
    check(virtual_bag::capacity_for_bag_type(0) == 0, "type 0 capacity");
    check(virtual_bag::capacity_for_bag_type(1) == 4, "type 1 capacity");
    check(virtual_bag::capacity_for_bag_type(2) == 8, "type 2 capacity");
    check(virtual_bag::capacity_for_bag_type(3) == 12, "type 3 capacity");
    check(virtual_bag::capacity_for_bag_type(4) == 16, "type 4 capacity");
    check(virtual_bag::capacity_for_bag_type(5) == 0, "unknown type capacity");

    extension_bag_test_set_active_count(0);
    check(virtual_bag::active_extension_bags() == 1, "active count lower clamp");
    extension_bag_test_set_active_count(1);
    check(virtual_bag::active_extension_bags() == 1, "active count minimum");
    extension_bag_test_set_active_count(5);
    check(virtual_bag::active_extension_bags() == 5, "active count default");
    extension_bag_test_set_active_count(8);
    check(virtual_bag::active_extension_bags() == 8, "active count maximum");
    check(virtual_bag::extension_logical_bag_last() == 13, "logical last at maximum");
    extension_bag_test_set_active_count(9);
    check(virtual_bag::active_extension_bags() == 8, "active count upper clamp");

    std::printf("bag_catalog_tests: %s\n", failures == 0 ? "passed" : "failed");
    return failures == 0 ? 0 : 1;
}
