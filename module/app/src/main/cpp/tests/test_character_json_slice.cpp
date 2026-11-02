#include "plugins/core.character/json_slice.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition) do { \
    if (condition) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (false)

using qol::features::core_character::JsonSlice;
using qol::features::core_character::json_member;

void test_truncated_strings() {
    JsonSlice value{};
    const char* truncated[] = {"{\"key\":\"unterminated}", "{\"key\":\"escape\\"};
    for (const char* json : truncated) {
        CHECK(!json_member(json, static_cast<uint32_t>(std::strlen(json)), "key", &value));
    }
}

void test_escaped_strings() {
    const char json[] = R"({"text":"escaped \\\" quote","tail":true})";
    JsonSlice value{};
    CHECK(json_member(json, sizeof(json) - 1, "text", &value));
    CHECK(std::string(value.data, value.size) == R"("escaped \\\" quote")");
    CHECK(json_member(json, sizeof(json) - 1, "tail", &value));
    CHECK(std::string(value.data, value.size) == "true");
}

void test_nested_values() {
    const char json[] = R"({"outer":{"array":[1,{"name":"inner"}]},"last":null})";
    JsonSlice value{};
    CHECK(json_member(json, sizeof(json) - 1, "outer", &value));
    CHECK(std::string(value.data, value.size) == R"({"array":[1,{"name":"inner"}]})");
    CHECK(json_member(value.data, value.size, "array", &value));
    CHECK(std::string(value.data, value.size) == "[1,{\"name\":\"inner\"}]");
}

void test_invalid_separators() {
    const char* invalid[] = {
        R"({"key":1 "other":2})",
        R"({"key":1;"other":2})",
        R"({"key":1,)"};
    JsonSlice value{};
    for (const char* json : invalid) {
        CHECK(!json_member(json, static_cast<uint32_t>(std::strlen(json)), "key", &value));
    }
}

void test_long_key() {
    const std::string key(4096, 'k');
    const std::string json = "{\"" + key + "\":{\"ok\":true}}";
    JsonSlice value{};
    CHECK(json_member(json.data(), static_cast<uint32_t>(json.size()), key.c_str(), &value));
    CHECK(std::string(value.data, value.size) == "{\"ok\":true}");
}

}  // namespace

int main() {
    test_truncated_strings();
    test_escaped_strings();
    test_nested_values();
    test_invalid_separators();
    test_long_key();
    std::printf("character_json_slice: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
