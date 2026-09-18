// infra/panels.h host 单测：注册/幂等/异 owner 拒绝/容量/注销/查找/边界。

#include "infra/panels.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "contracts/panels.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) ++g_pass; \
    else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

namespace {

using qol::contracts::PanelItemKind;
using qol::contracts::PanelItemSpec;
using qol::contracts::PanelProviderSpec;
using qol::infra::panels_find_provider;
using qol::infra::panels_item_at;
using qol::infra::panels_item_count;
using qol::infra::panels_register_item;
using qol::infra::panels_register_provider;
using qol::infra::panels_reset_for_test;
using qol::infra::panels_unregister_owner;

bool provider_open() { return true; }
void provider_close() {}
bool provider_active() { return false; }
const char* provider_reason() { return "blocked"; }

PanelItemSpec item_of(const char* id, const char* owner) {
    return PanelItemSpec{id, owner, "label", PanelItemKind::kToggle, "cfg.key", nullptr, nullptr};
}

PanelProviderSpec provider_of(const char* panel_id, const char* owner) {
    return PanelProviderSpec{panel_id, owner, &provider_open, &provider_close, &provider_active,
                             &provider_reason};
}

void reset() {
    panels_reset_for_test();
}

void test_register_and_duplicate() {
    reset();
    CHECK(panels_register_item(item_of("item:a", "owner_a")));
    CHECK(panels_item_count() == 1);
    CHECK(panels_register_item(item_of("item:a", "owner_a")));  // 同 owner 幂等
    CHECK(panels_item_count() == 1);
    CHECK(!panels_register_item(item_of("item:a", "owner_b")));  // 异 owner 拒绝
    CHECK(panels_item_count() == 1);
    CHECK(panels_register_item(item_of("item:b", "owner_b")));
    CHECK(panels_item_count() == 2);
}

void test_item_bounds() {
    reset();
    CHECK(!panels_register_item(item_of(nullptr, "owner")));
    CHECK(!panels_register_item(item_of("", "owner")));
    CHECK(!panels_register_item(item_of("item:a", nullptr)));
    CHECK(!panels_register_item(item_of("item:a", "")));
    CHECK(panels_item_count() == 0);
    CHECK(panels_item_at(0) == nullptr);

    CHECK(panels_register_item(item_of("item:a", "owner")));
    CHECK(panels_item_at(0) != nullptr);
    CHECK(panels_item_at(1) == nullptr);
    CHECK(std::strcmp(panels_item_at(0)->id, "item:a") == 0);
}

void test_item_capacity() {
    reset();
    static char ids[40][16];  // 注册表只保存指针：id 必须在测试内持久
    for (uint32_t i = 0; i < 32; ++i) {
        std::snprintf(ids[i], sizeof(ids[i]), "item:%u", i);
        CHECK(panels_register_item(item_of(ids[i], "owner")));
    }
    CHECK(panels_item_count() == 32);
    CHECK(!panels_register_item(item_of("item:overflow", "owner")));
    CHECK(panels_item_count() == 32);
    // 满容时同 id 同 owner 幂等仍成立。
    CHECK(panels_register_item(item_of(ids[0], "owner")));
}

void test_provider_register_find() {
    reset();
    CHECK(panels_register_provider(provider_of("panel:a", "owner_a")));
    CHECK(panels_register_provider(provider_of("panel:a", "owner_a")));  // 幂等
    CHECK(!panels_register_provider(provider_of("panel:a", "owner_b")));  // 异 owner
    CHECK(panels_find_provider("panel:a") != nullptr);
    CHECK(panels_find_provider("panel:a")->open == &provider_open);
    CHECK(panels_find_provider("panel:missing") == nullptr);
    CHECK(panels_find_provider(nullptr) == nullptr);
    CHECK(panels_find_provider("") == nullptr);
    CHECK(!panels_register_provider(provider_of(nullptr, "owner")));
    CHECK(!panels_register_provider(provider_of("", "owner")));
    CHECK(!panels_register_provider(provider_of("panel:b", nullptr)));
    CHECK(!panels_register_provider(provider_of("panel:b", "")));
}

void test_provider_capacity() {
    reset();
    static char ids[20][16];  // provider 只保存 panel_id 指针：需持久
    for (uint32_t i = 0; i < 16; ++i) {
        std::snprintf(ids[i], sizeof(ids[i]), "panel:%u", i);
        CHECK(panels_register_provider(provider_of(ids[i], "owner")));
    }
    CHECK(!panels_register_provider(provider_of("panel:overflow", "owner")));
    CHECK(panels_find_provider("panel:overflow") == nullptr);
}

void test_unregister_owner_preserves_order() {
    reset();
    CHECK(panels_register_item(item_of("item:a1", "owner_a")));
    CHECK(panels_register_item(item_of("item:b1", "owner_b")));
    CHECK(panels_register_item(item_of("item:a2", "owner_a")));
    CHECK(panels_register_item(item_of("item:c1", "owner_c")));
    CHECK(panels_register_provider(provider_of("panel:a", "owner_a")));
    CHECK(panels_register_provider(provider_of("panel:b", "owner_b")));

    panels_unregister_owner("owner_a");
    CHECK(panels_item_count() == 2);
    CHECK(std::strcmp(panels_item_at(0)->id, "item:b1") == 0);
    CHECK(std::strcmp(panels_item_at(1)->id, "item:c1") == 0);
    CHECK(panels_find_provider("panel:a") == nullptr);
    CHECK(panels_find_provider("panel:b") != nullptr);

    // 注销后 id 可被新 owner 复用。
    CHECK(panels_register_item(item_of("item:a1", "owner_d")));
    CHECK(panels_register_provider(provider_of("panel:a", "owner_d")));

    panels_unregister_owner(nullptr);
    panels_unregister_owner("");
    CHECK(panels_item_count() == 3);  // no-op
}

void test_reset() {
    reset();
    CHECK(panels_register_item(item_of("item:a", "owner")));
    CHECK(panels_register_provider(provider_of("panel:a", "owner")));
    panels_reset_for_test();
    CHECK(panels_item_count() == 0);
    CHECK(panels_item_at(0) == nullptr);
    CHECK(panels_find_provider("panel:a") == nullptr);
}

}  // namespace

int main() {
    test_register_and_duplicate();
    test_item_bounds();
    test_item_capacity();
    test_provider_register_find();
    test_provider_capacity();
    test_unregister_owner_preserves_order();
    test_reset();
    std::printf("infra_panels: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
