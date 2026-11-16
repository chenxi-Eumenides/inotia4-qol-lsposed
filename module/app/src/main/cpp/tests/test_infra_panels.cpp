// infra/panels.h host 单测：装配期声明校验、可用位翻转、枚举过滤、地址稳定性。

#include "infra/panels.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/capacity.h"
#include "contracts/panels.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using qol::contracts::PanelProviderSpec;
using qol::infra::panels_declare_provider;
using qol::infra::panels_find_provider;
using qol::infra::panels_provider_at;
using qol::infra::panels_provider_count;
using qol::infra::panels_reset_for_test;
using qol::infra::panels_set_owner_available;
using qol::infra::panels_validate_declaration;

bool provider_open() { return true; }
void provider_close() {}
bool provider_active() { return false; }
const char* provider_reason() { return "blocked"; }

PanelProviderSpec provider_of(const char* panel_id, const char* owner) {
    return PanelProviderSpec{panel_id, owner, &provider_open, &provider_close, &provider_active,
                             &provider_reason, nullptr};
}

// 声明表只保存指针，字符串必须是静态存储。
const char* const kPanelA = "panel:a";
const char* const kPanelB = "panel:b";
const char* const kPanelC = "panel:c";
const char* const kOwnerA = "owner_a";
const char* const kOwnerB = "owner_b";

void reset() { panels_reset_for_test(); }

void test_declare_and_duplicate() {
    reset();
    CHECK(panels_declare_provider(provider_of(kPanelA, kOwnerA)));
    CHECK(!panels_declare_provider(provider_of(kPanelA, kOwnerB)));  // 重复 panel_id
    CHECK(panels_provider_count() == 0);                             // 未置可用
    CHECK(panels_find_provider(kPanelA) == nullptr);
    panels_set_owner_available(kOwnerA, true);
    CHECK(panels_provider_count() == 1);
    CHECK(panels_find_provider(kPanelA) != nullptr);
    CHECK(panels_find_provider(kPanelA)->open == &provider_open);
    CHECK(panels_find_provider(kPanelB) == nullptr);
    CHECK(panels_find_provider(nullptr) == nullptr);
    CHECK(panels_find_provider("") == nullptr);
}

void test_declare_rejects_bad_fields() {
    reset();
    CHECK(!panels_declare_provider(provider_of(nullptr, kOwnerA)));
    CHECK(!panels_declare_provider(provider_of("", kOwnerA)));
    CHECK(!panels_declare_provider(provider_of(kPanelA, nullptr)));
    CHECK(!panels_declare_provider(provider_of(kPanelA, "")));
    PanelProviderSpec no_open = provider_of(kPanelA, kOwnerA);
    no_open.open = nullptr;
    CHECK(!panels_declare_provider(no_open));
    CHECK(panels_provider_count() == 0);
}

void test_validate_declaration() {
    reset();
    PanelProviderSpec batch[2] = {provider_of(kPanelA, kOwnerA), provider_of(kPanelB, kOwnerA)};
    CHECK(panels_validate_declaration(kOwnerA, batch, 2) == nullptr);
    // owner 与声明不一致
    CHECK(panels_validate_declaration(kOwnerB, batch, 2) != nullptr);
    // 数组内重复 panel_id
    batch[1] = provider_of(kPanelA, kOwnerA);
    CHECK(panels_validate_declaration(kOwnerA, batch, 2) != nullptr);
    // 单插件预算
    CHECK(panels_validate_declaration(
              kOwnerA, batch, qol::capacity::kMaxPanelProvidersPerPlugin + 1) != nullptr);
    // 声明后与表内冲突
    CHECK(panels_declare_provider(provider_of(kPanelC, kOwnerB)));
    CHECK(panels_validate_declaration(kOwnerA, batch, 2) != nullptr);
}

void test_capacity() {
    reset();
    static char ids[qol::capacity::kMaxPanelProviders + 1][16];
    for (uint32_t i = 0; i < qol::capacity::kMaxPanelProviders; ++i) {
        std::snprintf(ids[i], sizeof(ids[i]), "panel:%u", i);
        CHECK(panels_declare_provider(provider_of(ids[i], kOwnerA)));
    }
    CHECK(!panels_declare_provider(provider_of(ids[qol::capacity::kMaxPanelProviders], kOwnerA)));
    CHECK(panels_validate_declaration(kOwnerA, nullptr, 1) != nullptr);
}

void test_available_flip_filters_enumeration() {
    reset();
    CHECK(panels_declare_provider(provider_of(kPanelA, kOwnerA)));
    CHECK(panels_declare_provider(provider_of(kPanelB, kOwnerB)));
    panels_set_owner_available(kOwnerA, true);
    panels_set_owner_available(kOwnerB, true);
    CHECK(panels_provider_count() == 2);
    CHECK(std::strcmp(panels_provider_at(0)->panel_id, kPanelA) == 0);
    CHECK(std::strcmp(panels_provider_at(1)->panel_id, kPanelB) == 0);
    CHECK(panels_provider_at(2) == nullptr);

    panels_set_owner_available(kOwnerA, false);
    CHECK(panels_provider_count() == 1);
    CHECK(std::strcmp(panels_provider_at(0)->panel_id, kPanelB) == 0);
    CHECK(panels_find_provider(kPanelA) == nullptr);   // 不可用不对外开放
    CHECK(panels_find_provider(kPanelB) != nullptr);
    panels_set_owner_available(nullptr, false);        // no-op
    CHECK(panels_provider_count() == 1);
}

// 本次真机事故的回归：停用排在目标之前的 owner 后，先前取得的 provider 地址
// 必须仍指向同一 panel_id（旧实现会把整表左移，槽位改读后一项）。
void test_pointer_stability_across_owner_disable() {
    reset();
    CHECK(panels_declare_provider(provider_of(kPanelA, kOwnerA)));  // 先声明（如 item_editor）
    CHECK(panels_declare_provider(provider_of(kPanelB, kOwnerB)));  // 后声明（如 settings）
    CHECK(panels_declare_provider(provider_of(kPanelC, "owner_c")));
    panels_set_owner_available(kOwnerA, true);
    panels_set_owner_available(kOwnerB, true);
    panels_set_owner_available("owner_c", true);

    const PanelProviderSpec* settings_like = panels_find_provider(kPanelB);
    CHECK(settings_like != nullptr);

    // 停用排在前面的 owner：只翻可用位，不搬动任何条目。
    panels_set_owner_available(kOwnerA, false);
    CHECK(settings_like->panel_id != nullptr);
    CHECK(std::strcmp(settings_like->panel_id, kPanelB) == 0);
    CHECK(panels_find_provider(kPanelB) == settings_like);
    CHECK(panels_find_provider(kPanelA) == nullptr);

    // 重新启用后地址仍然不变。
    panels_set_owner_available(kOwnerA, true);
    CHECK(panels_find_provider(kPanelA) != nullptr);
    CHECK(panels_find_provider(kPanelB) == settings_like);
    CHECK(panels_provider_count() == 3);
}

void test_reset() {
    reset();
    CHECK(panels_declare_provider(provider_of(kPanelA, kOwnerA)));
    panels_set_owner_available(kOwnerA, true);
    CHECK(panels_provider_count() == 1);
    panels_reset_for_test();
    CHECK(panels_provider_count() == 0);
    CHECK(panels_provider_at(0) == nullptr);
    CHECK(panels_find_provider(kPanelA) == nullptr);
}

}  // namespace

int main() {
    test_declare_and_duplicate();
    test_declare_rejects_bad_fields();
    test_validate_declaration();
    test_capacity();
    test_available_flip_filters_enumeration();
    test_pointer_stability_across_owner_disable();
    test_reset();
    std::printf("infra_panels_tests: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
