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

    check(virtual_bag::kMaxExtensionBags == 128, "maximum bag count");
    check(virtual_bag::kSlotCount == 16, "slot count");
    check(virtual_bag::kRenderedExtensionBagTabsPerColumn == 5, "tabs per column");
    check(virtual_bag::kMaxRenderedExtensionBagTabs == 20, "maximum rendered tabs");
    for (const int active : {1, 5, 6, 20, 21, 128}) {
        const int expected_tabs = active > 20 ? 20 : active;
        const int expected_columns = (expected_tabs + 4) / 5;
        check(virtual_bag::rendered_extension_bag_tabs(active) == expected_tabs,
              "rendered tab count boundary");
        check(virtual_bag::rendered_extension_bag_columns(active) == expected_columns,
              "rendered column count boundary");
    }
    check(virtual_bag::rendered_extension_bag_tabs(0) == 1, "rendered tabs minimum");
    check(virtual_bag::rendered_tab_column(5) == 1 && virtual_bag::rendered_tab_row(5) == 0,
          "second column starts at index five");
    check(virtual_bag::rendered_tab_column(19) == 3 && virtual_bag::rendered_tab_row(19) == 4,
          "fourth column ends at index nineteen");
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
    extension_bag_test_set_active_count(128);
    check(virtual_bag::active_extension_bags() == 128, "active count maximum");
    check(virtual_bag::extension_logical_bag_last() == 133, "logical last at maximum");
    extension_bag_test_set_active_count(129);
    check(virtual_bag::active_extension_bags() == 128, "active count upper clamp");

    std::printf("bag_catalog_tests: %s\n", failures == 0 ? "passed" : "failed");
    return failures == 0 ? 0 : 1;
}
