#include "plugins/extension_bag/http/query.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

std::string extension_bag_api_status_json_impl() {
    return "{\"enabled\":true,\"state\":{\"mode\":\"module\"}}";
}

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(condition)                                                                          \
    do {                                                                                          \
        if (condition) {                                                                          \
            ++g_pass;                                                                             \
        } else {                                                                                  \
            ++g_fail;                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                    \
        }                                                                                         \
    } while (false)

constexpr char kStatusJson[] = "{\"enabled\":true,\"state\":{\"mode\":\"module\"}}";
constexpr char kStatusPath[] = "/api/debug/extension_bag/status";

void test_status_query_returns_owner_body_verbatim() {
    using qol::contracts::HttpMethod;
    using qol::contracts::HttpReq;
    using qol::contracts::HttpRes;

    const HttpReq request{HttpMethod::kGet, kStatusPath, sizeof(kStatusPath) - 1,
                          nullptr, 0, nullptr, 0, nullptr, 0};
    std::array<uint8_t, sizeof(kStatusJson)> output{};
    output.fill(0x7f);
    HttpRes response{output.data(), static_cast<uint32_t>(sizeof(kStatusJson) - 1), 0, 0};

    const QolResult result = qol::features::extension_bag::status_query_handler(
        &request, &response, nullptr);

    CHECK(qol::contracts::succeeded(result));
    CHECK(response.status == 200);
    CHECK(response.body_size == sizeof(kStatusJson) - 1);
    CHECK(std::memcmp(output.data(), kStatusJson, sizeof(kStatusJson) - 1) == 0);
    CHECK(output[sizeof(kStatusJson) - 1] == 0x7f);
}

}  // namespace

int main() {
    test_status_query_returns_owner_body_verbatim();
    std::printf("extension_bag_status_query_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
