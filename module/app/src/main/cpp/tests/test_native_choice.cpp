#include "feature/ui/native_choice.h"

#include <cstdint>
#include <cstdio>

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)

void on_select(int, void*) {}

NativeChoiceSpec valid_spec(int count = 2, int close_index = 1) {
    static const char* const items[] = {"one", "two", "three", "four", "five", "six"};
    return {items, count, "title", close_index, &on_select, nullptr};
}

void test_spec_bounds() {
    CHECK(!native_choice_detail::valid_spec(valid_spec(0)));
    CHECK(native_choice_detail::valid_spec(valid_spec(1, -1)));
    CHECK(native_choice_detail::valid_spec(valid_spec(6, 5)));
    CHECK(!native_choice_detail::valid_spec(valid_spec(7)));
    CHECK(!native_choice_detail::valid_spec(valid_spec(2, 2)));
}

void test_repeated_open() {
    const NativeChoiceSpec spec = valid_spec();
    CHECK(native_choice_detail::can_open(false, spec));
    CHECK(!native_choice_detail::can_open(true, spec));
}

void test_close_index() {
    CHECK(native_choice_detail::is_close_index(1, 1));
    CHECK(!native_choice_detail::is_close_index(0, 1));
    CHECK(!native_choice_detail::is_close_index(0, -1));
    CHECK(native_choice_detail::valid_spec(valid_spec(1, 0)));
}

void test_address_ownership_range() {
    constexpr uintptr_t begin = 0x1000;
    CHECK(native_choice_detail::address_in_range(begin, begin, 256));
    CHECK(native_choice_detail::address_in_range(begin + 255, begin, 256));
    CHECK(!native_choice_detail::address_in_range(begin + 256, begin, 256));
    CHECK(!native_choice_detail::address_in_range(begin - 1, begin, 256));
    CHECK(!native_choice_detail::address_in_range(begin, begin, 0));
}

}  // namespace

int main() {
    test_spec_bounds();
    test_repeated_open();
    test_close_index();
    test_address_ownership_range();
    std::printf("native_choice_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
