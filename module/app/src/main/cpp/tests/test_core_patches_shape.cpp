#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

#ifndef QOL_CORE_PATCHES_SOURCE
#error "QOL_CORE_PATCHES_SOURCE must point to host/core_patches.cpp"
#endif

int main() {
    std::ifstream input(QOL_CORE_PATCHES_SOURCE);
    assert(input.good());
    const std::string source((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    assert(source.find("P_HUB_LOGIN_SKIP") != std::string::npos);
    assert(source.find("kOwner = \"host\"") != std::string::npos);
    assert(source.find("kReason = \"Hive 登录跳过\"") != std::string::npos);

    static const char* kS2Claims[] = {
        "P_UISTORE_BUTTON_SELL_EXE_D18D0_ARG", "P_UISTORE_BUTTON_SELL_EXE_D18D0_CALL",
        "P_UISTORE_BUTTON_SELL_EXE_D18E8_ARG", "P_UISTORE_BUTTON_SELL_EXE_D18E8_CALL",
        "P_UISTORE_SELL_ITEM_D26B8_ARG", "P_UISTORE_SELL_ITEM_D26B8_CALL",
        "P_ITEMSYSTEM_DIVIDE_108458_ARG", "P_ITEMSYSTEM_DIVIDE_108458_CALL",
        "P_ITEMSYSTEM_DIVIDE_10848C_ARG", "P_ITEMSYSTEM_DIVIDE_10848C_CALL",
    };
    for (const char* claim : kS2Claims) assert(source.find(claim) == std::string::npos);
    assert(source.find("P_SAVE_SAVE_INVENTORY_BITEND") != std::string::npos);
    assert(source.find("P_SAVE_LOAD_INVENTORY_BITEND") != std::string::npos);
    assert(source.find("999 数量组存档位段守卫（常驻）") != std::string::npos);
    return 0;
}
