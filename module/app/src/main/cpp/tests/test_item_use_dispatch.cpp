#include <cassert>
#include <cstdint>

#include "plugins/core.item/api/item_use.h"

namespace {

using qol::contracts::QolItemUseEvent;

int g_calls = 0;
void* g_last_character = nullptr;
int32_t g_last_flag = 0;

void counting_handler(const QolItemUseEvent* event, void* ctx) {
    ++g_calls;
    g_last_character = event->character;
    g_last_flag = event->flag;
    assert(ctx == reinterpret_cast<void*>(0x55));
}

void reentrant_handler(const QolItemUseEvent*, void* ctx) {
    ++g_calls;
    QolItemUseEvent nested{};
    nested.struct_size = sizeof(nested);
    qol::features::core_item::item_use_dispatch(nested);
    assert(ctx == nullptr);
}

QolItemUseEvent make_event() {
    QolItemUseEvent event{};
    event.struct_size = sizeof(event);
    event.character = reinterpret_cast<void*>(0x1234);
    event.flag = 7;
    event.result = 1;
    return event;
}

}  // namespace

int main() {
    using qol::features::core_item::kItemUseApi;
    using qol::features::core_item::item_use_dispatch;
    using qol::features::core_item::item_use_reset;

    assert(qol::contracts::item_use_api_size_valid(&kItemUseApi));
    item_use_reset();

    uint64_t first = 0;
    assert(kItemUseApi.subscribe_post("plugin.a", &counting_handler,
                                      reinterpret_cast<void*>(0x55), &first).ok == kQolOk);
    assert(first != 0);

    uint64_t duplicate = 0;
    assert(kItemUseApi.subscribe_post("plugin.a", &counting_handler, nullptr, &duplicate).ok !=
           kQolOk);
    assert(duplicate == 0);

    QolItemUseEvent event = make_event();
    item_use_dispatch(event);
    assert(g_calls == 1);
    assert(g_last_character == event.character);
    assert(g_last_flag == 7);

    assert(kItemUseApi.unsubscribe_post(first).ok == kQolOk);
    item_use_dispatch(event);
    assert(g_calls == 1);
    assert(kItemUseApi.unsubscribe_post(first).ok != kQolOk);

    // 回调内再次 dispatch 不递归。
    g_calls = 0;
    uint64_t reentrant = 0;
    assert(kItemUseApi.subscribe_post("plugin.reentrant", &reentrant_handler, nullptr,
                                      &reentrant).ok == kQolOk);
    item_use_dispatch(event);
    assert(g_calls == 1);
    assert(kItemUseApi.unsubscribe_post(reentrant).ok == kQolOk);

    // 容量上限 8；同一 owner/fn 之外的组合占满后返回 busy。
    item_use_reset();
    const char* owners[8] = {"plugin.full0", "plugin.full1", "plugin.full2", "plugin.full3",
                             "plugin.full4", "plugin.full5", "plugin.full6", "plugin.full7"};
    uint64_t ids[8] = {};
    for (uint32_t i = 0; i < 8; ++i) {
        assert(kItemUseApi.subscribe_post(owners[i], &counting_handler,
                                          reinterpret_cast<void*>(0x55), &ids[i]).ok == kQolOk);
    }
    uint64_t overflow = 0;
    assert(kItemUseApi.subscribe_post("plugin.full8", &reentrant_handler, nullptr, &overflow).ok !=
           kQolOk);
    item_use_reset();
    g_calls = 0;
    item_use_dispatch(event);
    assert(g_calls == 0);  // reset 后无订阅者

    return 0;
}
