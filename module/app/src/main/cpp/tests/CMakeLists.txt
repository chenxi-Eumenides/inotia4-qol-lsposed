cmake_minimum_required(VERSION 3.16)
project(host_tests CXX)

# 独立 host 单测构建（不并入主 CMakeLists：主构建绑定 NDK 工具链，仅面向 arm64/arm）。
# 编译真实被测源文件（纯 STL 逻辑），以 stubs/ 覆盖 Android 专属头，ctest 运行不依赖设备。

enable_testing()

add_test(NAME game_host_legacy_surface_negative
    COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -I${CMAKE_CURRENT_SOURCE_DIR}/..
            -fsyntax-only ${CMAKE_CURRENT_SOURCE_DIR}/negative/game_host_legacy_surface.cpp)
set_tests_properties(game_host_legacy_surface_negative PROPERTIES WILL_FAIL TRUE)

add_test(NAME game_adapter_link_negative
    COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -I${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_SOURCE_DIR}/negative/game_adapter_link.cpp -o
            ${CMAKE_CURRENT_BINARY_DIR}/game_adapter_link_negative)
set_tests_properties(game_adapter_link_negative PROPERTIES WILL_FAIL TRUE)

find_package(JNI REQUIRED)

set(CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# 注：game_tiles.cpp 不在此列表——test_host.cpp 直接 #include 它，
# 以访问匿名命名空间的 base64_decode/parse_int_field（内部纯函数）。
add_executable(host_tests
    test_host.cpp
    test_stubs.cpp
    ${CPP_DIR}/plugins/inventory_hooks/inventory_find_item_poc.cpp
    ${CPP_DIR}/patch/call_patch.cpp
    ${CPP_DIR}/infra/log/qol_log.cpp
    ${CPP_DIR}/game/json/game_json.cpp
    ${CPP_DIR}/game/nav/game_nav.cpp
    ${CPP_DIR}/game/access/save_preflight.cpp
    ${CPP_DIR}/plugins/save_backup/bundle.cpp
    ${CPP_DIR}/plugins/world_teleport/rules.cpp
)

add_executable(item_facts_plugin_tests
    test_item_facts_plugin.cpp
    ${CPP_DIR}/plugins/core.item/plugin.cpp
    ${CPP_DIR}/plugins/core.item/json_lite.cpp
    ${CPP_DIR}/plugins/core.item/response.cpp
)
target_include_directories(item_facts_plugin_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(item_facts_plugin_tests PRIVATE cxx_std_17)
set_target_properties(item_facts_plugin_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME item_facts_plugin_tests COMMAND item_facts_plugin_tests)

add_executable(save_facts_plugin_tests
    test_save_facts_plugin.cpp
    ${CPP_DIR}/plugins/core.save/plugin.cpp
)
target_include_directories(save_facts_plugin_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(save_facts_plugin_tests PRIVATE cxx_std_17)
set_target_properties(save_facts_plugin_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME save_facts_plugin_tests COMMAND save_facts_plugin_tests)

add_executable(craft_facts_plugin_tests
    test_craft_facts_plugin.cpp
    ${CPP_DIR}/plugins/core.craft/plugin.cpp
)
target_include_directories(craft_facts_plugin_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(craft_facts_plugin_tests PRIVATE cxx_std_17)
set_target_properties(craft_facts_plugin_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME craft_facts_plugin_tests COMMAND craft_facts_plugin_tests)

target_include_directories(host_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
    ${CPP_DIR}/game/read
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)

target_compile_features(host_tests PRIVATE cxx_std_17)

add_test(NAME host_tests COMMAND host_tests)

add_executable(stage4_hook_tests
    test_inventory_hook_stage4.cpp
    ${CPP_DIR}/plugins/inventory_hooks/inventory_hook_stage4.cpp
)
target_include_directories(stage4_hook_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(stage4_hook_tests PRIVATE cxx_std_17)
add_test(NAME stage4_hook_tests COMMAND stage4_hook_tests)

add_executable(attribute_quality_tests
    test_attribute_quality.cpp
    ${CPP_DIR}/plugins/attribute_quality/rules.cpp
)
target_include_directories(attribute_quality_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(attribute_quality_tests PRIVATE cxx_std_17)
add_test(NAME attribute_quality_tests COMMAND attribute_quality_tests)

# 自定义合成配方纯逻辑 host 测试（档位互逆 / 目录映射 / 表注入字节；末尾追加）
add_executable(custom_recipe_tests
    test_custom_recipe.cpp
    ${CPP_DIR}/plugins/attribute_quality/rules.cpp
    ${CPP_DIR}/plugins/custom_recipe/rules.cpp
    ${CPP_DIR}/plugins/custom_recipe/catalog.cpp
    ${CPP_DIR}/plugins/custom_recipe/table.cpp
)
target_include_directories(custom_recipe_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(custom_recipe_tests PRIVATE cxx_std_17)
add_test(NAME custom_recipe_tests COMMAND custom_recipe_tests)

add_executable(autosell_rules_tests
    test_autosell_rules.cpp
    ${CPP_DIR}/plugins/autosell/rules.cpp
)
target_include_directories(autosell_rules_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(autosell_rules_tests PRIVATE cxx_std_17)
add_test(NAME autosell_rules_tests COMMAND autosell_rules_tests)

# 自动出售阶段 A 纯逻辑 host 测试（按帧去重 + 物品编码分支）
add_executable(infra_data_tests
    test_infra_data.cpp
)
target_include_directories(infra_data_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(infra_data_tests PRIVATE cxx_std_17)
add_test(NAME infra_data_tests COMMAND infra_data_tests)

# 自动出售 sidecar 配置纯序列化 host 测试（末尾追加）
add_executable(autosell_store_tests
    test_autosell_store.cpp
)
target_include_directories(autosell_store_tests PRIVATE
    ${CPP_DIR}
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(autosell_store_tests PRIVATE cxx_std_17)
add_test(NAME autosell_store_tests COMMAND autosell_store_tests)

# 状态转换派发纯逻辑 host 测试（末尾追加）
add_executable(transition_dispatch_tests
    test_transition_dispatch.cpp
)
target_include_directories(transition_dispatch_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(transition_dispatch_tests PRIVATE cxx_std_17)
add_test(NAME transition_dispatch_tests COMMAND transition_dispatch_tests)

# 统一日志系统 P0 纯格式化/限流 host 测试（末尾追加）
add_executable(qol_log_tests
    test_qol_log.cpp
    ${CPP_DIR}/infra/log/qol_log.cpp
)
target_include_directories(qol_log_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
)
target_compile_features(qol_log_tests PRIVATE cxx_std_17)
add_test(NAME qol_log_tests COMMAND qol_log_tests)

# libgame.so 变体对照表运行时查表测试（末尾追加）
# test_game_variant.cpp 直接 #include game_variant.cpp 以访问匿名命名空间 MD5 内部函数，
# 故不再单独编译该 .cpp；qol_log.cpp 提供日志符号，Android 头由 stubs/ 覆盖。
add_executable(game_variant_tests
    test_game_variant.cpp
    ${CPP_DIR}/infra/log/qol_log.cpp
)
target_include_directories(game_variant_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
)
target_compile_features(game_variant_tests PRIVATE cxx_std_17)
add_test(NAME game_variant_tests COMMAND game_variant_tests)

# 功能抽象层纯逻辑测试（末尾追加）
add_executable(game_feature_tests
    test_game_feature.cpp
    ${CPP_DIR}/game/feature/game_feature.cpp
    ${CPP_DIR}/game/variants/game_variant.cpp
    ${CPP_DIR}/infra/log/qol_log.cpp
)
target_include_directories(game_feature_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
)
target_compile_features(game_feature_tests PRIVATE cxx_std_17)
add_test(NAME game_feature_tests COMMAND game_feature_tests)

# wh4 容器可用性判定纯逻辑测试（末尾追加）
add_executable(save_warehouse_validate_tests
    test_save_warehouse_validate.cpp
    ${CPP_DIR}/plugins/save_backup/bundle.cpp
)
target_include_directories(save_warehouse_validate_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(save_warehouse_validate_tests PRIVATE cxx_std_17)
add_test(NAME save_warehouse_validate_tests COMMAND save_warehouse_validate_tests)

# 原生选择框插件 host 测试：真实 feature.cpp、同步 push→init hook 回调、失败回滚与生命周期门禁。
add_executable(native_choice_tests
    test_native_choice.cpp
    ${CPP_DIR}/plugins/core.custom_ui/feature.cpp
    ${CPP_DIR}/plugins/core.custom_ui/plugin.cpp
    ${CPP_DIR}/plugins/core.custom_ui/text_mechanism.cpp
    ${CPP_DIR}/plugins/core.custom_ui/ui_components.cpp
    ${CPP_DIR}/plugins/core.custom_ui/panels/stack.cpp
)
target_include_directories(native_choice_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(native_choice_tests PRIVATE cxx_std_17)
# contracts/plugin.h 字段名 requires 在 C++20 是关键字：显式锚定 C++17（与 Android 构建一致）。
set_target_properties(native_choice_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME native_choice_tests COMMAND native_choice_tests)
set_tests_properties(native_choice_tests PROPERTIES TIMEOUT 10)

# 角色属性面板标签中文化白名单 host 测试（纯表逻辑，header-inline，末尾追加）
add_executable(module_text_tests
    test_module_text.cpp
)
target_include_directories(module_text_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(module_text_tests PRIVATE cxx_std_17)
# 显式锚定 C++17：plugins/character_panel_i18n/feature.h 经 contracts/plugin.h 使用字段名 requires。
set_target_properties(module_text_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME module_text_tests COMMAND module_text_tests)

# P4 阶段 A4：共享资源分派 host 测试（§4.5；末尾追加）
add_executable(hooks_dispatch_tests
    test_hooks_dispatch.cpp
    ${CPP_DIR}/hooks/dispatch.cpp
)
target_include_directories(hooks_dispatch_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(hooks_dispatch_tests PRIVATE cxx_std_17)
set_target_properties(hooks_dispatch_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(hooks_dispatch_tests PRIVATE Threads::Threads)
add_test(NAME hooks_dispatch_tests COMMAND hooks_dispatch_tests)

# 简单模式纯逻辑 host 测试（阵营矩阵 / 倍率 / 边界钳制，末尾追加）
add_executable(simple_mode_rules_tests
    test_simple_mode_rules.cpp
    ${CPP_DIR}/plugins/simple_mode/rules.cpp
)
target_include_directories(simple_mode_rules_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(simple_mode_rules_tests PRIVATE cxx_std_17)
add_test(NAME simple_mode_rules_tests COMMAND simple_mode_rules_tests)

# 模块 wrapper enter → 原版面板 VMA 反查注册表 host 测试（末尾追加）
add_executable(popup_enter_alias_tests
    test_popup_enter_alias.cpp
    ${CPP_DIR}/infra/popup_enter_alias.cpp
)
target_include_directories(popup_enter_alias_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(popup_enter_alias_tests PRIVATE cxx_std_17)
add_test(NAME popup_enter_alias_tests COMMAND popup_enter_alias_tests)

# P1 最小骨架：infra 骨架 host 单测（快照/命令队列/事件/能力与状态，末尾追加）
# 一个测试文件一个可执行目标（每个文件自带 main）；test_reset.cpp 提供聚合重置。
set(INFRA_TEST_SOURCES
    ${CPP_DIR}/infra/command_queue.cpp
    ${CPP_DIR}/infra/runtime.cpp
    ${CPP_DIR}/infra/events.cpp
    ${CPP_DIR}/infra/capabilities.cpp
    ${CPP_DIR}/infra/status.cpp
    ${CPP_DIR}/infra/test_reset.cpp
)
find_package(Threads REQUIRED)
add_executable(command_queue_overflow_tests
    test_command_queue_overflow.cpp
    ${CPP_DIR}/infra/command_queue.cpp
)
target_include_directories(command_queue_overflow_tests PRIVATE ${CPP_DIR})
target_compile_features(command_queue_overflow_tests PRIVATE cxx_std_17)
target_link_libraries(command_queue_overflow_tests PRIVATE Threads::Threads)
add_test(NAME command_queue_overflow_tests COMMAND command_queue_overflow_tests)

foreach(infra_case snapshot command_queue events capabilities_status)
    add_executable(infra_${infra_case}_tests
        test_infra_${infra_case}.cpp
        ${INFRA_TEST_SOURCES}
    )
    target_include_directories(infra_${infra_case}_tests PRIVATE
        ${CPP_DIR}
    )
    target_compile_features(infra_${infra_case}_tests PRIVATE cxx_std_17)
    target_link_libraries(infra_${infra_case}_tests PRIVATE Threads::Threads)
    add_test(NAME infra_${infra_case}_tests COMMAND infra_${infra_case}_tests)
endforeach()

# P2 纵向样板基础层：hooks 资源登记/安装 host 测试（末尾追加）
# 显式锚定 C++17：contracts/plugin.h 按 §3.3 使用字段名 requires，而 host 编译器
# 默认 C++20（requires 是关键字）；Android 构建本身固定 -std=c++17，这里与之一致。
add_executable(hooks_registry_tests
    test_hooks_registry.cpp
    ${CPP_DIR}/hooks/registry.cpp
    ${CPP_DIR}/hooks/install.cpp
)
target_include_directories(hooks_registry_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(hooks_registry_tests PRIVATE cxx_std_17)
set_target_properties(hooks_registry_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME hooks_registry_tests COMMAND hooks_registry_tests)

# P2 纵向样板基础层：infra 配置与命令端口 host 测试（末尾追加）
add_executable(infra_config_commands_tests
    test_infra_config_commands.cpp
    ${CPP_DIR}/infra/config.cpp
    ${CPP_DIR}/infra/commands.cpp
    ${CPP_DIR}/infra/command_queue.cpp
    ${CPP_DIR}/infra/events.cpp
)
target_include_directories(infra_config_commands_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(infra_config_commands_tests PRIVATE cxx_std_17)
set_target_properties(infra_config_commands_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(infra_config_commands_tests PRIVATE Threads::Threads)
add_test(NAME infra_config_commands_tests COMMAND infra_config_commands_tests)

# P2 纵向样板：host 插件注册表与声明式安装 host 测试（fake 后端，末尾追加）
# 显式锚定 C++17：contracts/plugin.h 按 §3.3 使用字段名 requires，而 host 编译器
# 默认 C++20（requires 是关键字）；Android 构建本身固定 -std=c++17，这里与之一致。
add_executable(plugin_registry_tests
    test_plugin_registry.cpp
    test_stubs.cpp
    test_game_port_stubs.cpp
    ${CPP_DIR}/host/plugin_registry.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
    ${CPP_DIR}/host/item_count_facade.cpp
    ${CPP_DIR}/game/adapter_ports/item_count_facade.cpp
    ${CPP_DIR}/hooks/registry.cpp
    ${CPP_DIR}/hooks/install.cpp
    ${CPP_DIR}/hooks/transaction.cpp
    ${CPP_DIR}/game/access/patches.cpp
    ${CPP_DIR}/game/read/item_class.cpp
    ${CPP_DIR}/game/access/ptr_slots.cpp
    ${CPP_DIR}/game/access/ui_equip.cpp
    ${CPP_DIR}/patch/claims.cpp
    ${CPP_DIR}/patch/instr_patch.cpp
    ${CPP_DIR}/patch/ptr_slot.cpp
    ${CPP_DIR}/infra/status.cpp
    ${CPP_DIR}/infra/capabilities.cpp
    ${CPP_DIR}/infra/config.cpp
    ${CPP_DIR}/infra/events.cpp
    ${CPP_DIR}/infra/log/qol_log.cpp
)
target_include_directories(plugin_registry_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
    ${CPP_DIR}/game/read
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(plugin_registry_tests PRIVATE cxx_std_17)
set_target_properties(plugin_registry_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(plugin_registry_tests PRIVATE Threads::Threads)
add_test(NAME plugin_registry_tests COMMAND plugin_registry_tests)

# P3 Lane A：资源事务引擎 host 测试（运行期记录/在途/隔离/物理释放/帧步进事务，末尾追加）
# 显式锚定 C++17：contracts/plugin.h 按 §3.3 使用字段名 requires，而 host 编译器
# 默认 C++20（requires 是关键字）；Android 构建本身固定 -std=c++17，这里与之一致。
add_executable(hooks_transaction_tests
    test_hooks_transaction.cpp
    ${CPP_DIR}/hooks/registry.cpp
    ${CPP_DIR}/hooks/install.cpp
    ${CPP_DIR}/hooks/transaction.cpp
)
target_include_directories(hooks_transaction_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(hooks_transaction_tests PRIVATE cxx_std_17)
set_target_properties(hooks_transaction_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME hooks_transaction_tests COMMAND hooks_transaction_tests)

# ELF export resolution fixture uses a real .dynsym and SysV hash table.
add_library(plugin_export_fixture SHARED test_plugin_export_fixture.cpp)
set_target_properties(plugin_export_fixture PROPERTIES
    OUTPUT_NAME game
    CXX_VISIBILITY_PRESET hidden
)
target_link_options(plugin_export_fixture PRIVATE -Wl,--hash-style=sysv)
add_executable(symbol_resolver_tests
    test_symbol_resolver.cpp
    ${CPP_DIR}/game/symbols/symbol_resolver.cpp
)
target_include_directories(symbol_resolver_tests PRIVATE ${CPP_DIR})
target_compile_features(symbol_resolver_tests PRIVATE cxx_std_17)
set_target_properties(symbol_resolver_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(symbol_resolver_tests PRIVATE
    QOL_PLUGIN_EXPORT_FIXTURE="$<TARGET_FILE:plugin_export_fixture>")
target_link_libraries(symbol_resolver_tests PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(symbol_resolver_tests plugin_export_fixture)
add_test(NAME symbol_resolver_tests COMMAND symbol_resolver_tests)

# P3 Lane B：生命周期全路径 host 测试（真实 registry/tx/status/config + 假 tx 依赖，末尾追加）
# 显式锚定 C++17：contracts/plugin.h 字段名 requires 在 C++20 是关键字。
add_executable(plugin_lifecycle_tests
    test_plugin_lifecycle.cpp
    test_stubs.cpp
    test_game_port_stubs.cpp
    ${CPP_DIR}/host/plugin_registry.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
    ${CPP_DIR}/hooks/registry.cpp
    ${CPP_DIR}/hooks/install.cpp
    ${CPP_DIR}/hooks/transaction.cpp
    ${CPP_DIR}/game/access/patches.cpp
    ${CPP_DIR}/game/read/item_class.cpp
    ${CPP_DIR}/host/item_count_facade.cpp
    ${CPP_DIR}/game/adapter_ports/item_count_facade.cpp
    ${CPP_DIR}/game/access/ptr_slots.cpp
    ${CPP_DIR}/game/access/ui_equip.cpp
    ${CPP_DIR}/patch/claims.cpp
    ${CPP_DIR}/patch/instr_patch.cpp
    ${CPP_DIR}/patch/ptr_slot.cpp
    ${CPP_DIR}/infra/status.cpp
    ${CPP_DIR}/infra/capabilities.cpp
    ${CPP_DIR}/infra/config.cpp
    ${CPP_DIR}/infra/events.cpp
    ${CPP_DIR}/infra/log/qol_log.cpp
)
target_include_directories(plugin_lifecycle_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
    ${CPP_DIR}/game/read
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(plugin_lifecycle_tests PRIVATE cxx_std_17)
set_target_properties(plugin_lifecycle_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(plugin_lifecycle_tests PRIVATE Threads::Threads)
add_test(NAME plugin_lifecycle_tests COMMAND plugin_lifecycle_tests)

# P4 阶段 A3：能力时序与撤销级联 host 测试（真实 registry/capabilities/status/config，末尾追加）
# 显式锚定 C++17：contracts/plugin.h 字段名 requires 在 C++20 是关键字。
add_executable(capability_lifecycle_tests
    test_capability_lifecycle.cpp
    test_stubs.cpp
    test_game_port_stubs.cpp
    ${CPP_DIR}/host/plugin_registry.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
    ${CPP_DIR}/hooks/registry.cpp
    ${CPP_DIR}/hooks/install.cpp
    ${CPP_DIR}/hooks/transaction.cpp
    ${CPP_DIR}/game/access/patches.cpp
    ${CPP_DIR}/game/read/item_class.cpp
    ${CPP_DIR}/host/item_count_facade.cpp
    ${CPP_DIR}/game/adapter_ports/item_count_facade.cpp
    ${CPP_DIR}/game/access/ptr_slots.cpp
    ${CPP_DIR}/game/access/ui_equip.cpp
    ${CPP_DIR}/patch/claims.cpp
    ${CPP_DIR}/patch/instr_patch.cpp
    ${CPP_DIR}/patch/ptr_slot.cpp
    ${CPP_DIR}/infra/status.cpp
    ${CPP_DIR}/infra/capabilities.cpp
    ${CPP_DIR}/infra/config.cpp
    ${CPP_DIR}/infra/events.cpp
    ${CPP_DIR}/infra/log/qol_log.cpp
)
target_include_directories(capability_lifecycle_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
    ${CPP_DIR}/game/read
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(capability_lifecycle_tests PRIVATE cxx_std_17)
set_target_properties(capability_lifecycle_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(capability_lifecycle_tests PRIVATE Threads::Threads)
add_test(NAME capability_lifecycle_tests COMMAND capability_lifecycle_tests)

# P4 阶段 A1：patch/ 机制层 host 测试（claims + instr_patch + game patch 点，假原语，末尾追加）
# 显式锚定 C++17：contracts/plugin.h 字段名 requires 在 C++20 是关键字。
add_executable(patch_layer_tests
    test_patch_layer.cpp
    ${CPP_DIR}/patch/claims.cpp
    ${CPP_DIR}/patch/instr_patch.cpp
    ${CPP_DIR}/patch/ptr_slot.cpp
    ${CPP_DIR}/game/access/patches.cpp
    ${CPP_DIR}/game/read/item_class.cpp
    ${CPP_DIR}/host/item_count_facade.cpp
    ${CPP_DIR}/game/adapter_ports/item_count_facade.cpp
)
target_include_directories(patch_layer_tests PRIVATE
    ${CPP_DIR}
    ${CPP_DIR}/game/read
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(patch_layer_tests PRIVATE cxx_std_17)
set_target_properties(patch_layer_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME patch_layer_tests COMMAND patch_layer_tests)

# D2a-3a：extension_bag 契约 + 声明式站点登记 host 测试（假 g_base；末尾追加）
add_executable(extension_bag_contract_tests
    test_extension_bag_contract.cpp
    ${CPP_DIR}/game/access/patches.cpp
    ${CPP_DIR}/game/read/item_class.cpp
    ${CPP_DIR}/host/item_count_facade.cpp
    ${CPP_DIR}/game/adapter_ports/item_count_facade.cpp
    ${CPP_DIR}/game/access/ptr_slots.cpp
    ${CPP_DIR}/game/access/ui_equip.cpp
)
target_include_directories(extension_bag_contract_tests PRIVATE
    ${CPP_DIR}
    ${CPP_DIR}/game/read
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(extension_bag_contract_tests PRIVATE cxx_std_17)
set_target_properties(extension_bag_contract_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME extension_bag_contract_tests COMMAND extension_bag_contract_tests)

# D1-a2：PopupState state 表注入机制 host 测试（假 state 表；占用/幂等/还原；末尾追加）
add_executable(hooks_popup_state_tests
    test_hooks_popup_state.cpp
    ${CPP_DIR}/hooks/popup_state.cpp
    ${CPP_DIR}/game/access/popup_state.cpp
    ${CPP_DIR}/hooks/registry.cpp
    ${CPP_DIR}/infra/popup_enter_alias.cpp
)
target_include_directories(hooks_popup_state_tests PRIVATE
    ${CPP_DIR}
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(hooks_popup_state_tests PRIVATE cxx_std_17)
set_target_properties(hooks_popup_state_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME hooks_popup_state_tests COMMAND hooks_popup_state_tests)

# P4 阶段 A1：push_all 纯逻辑 host 测试（party_decision 矩阵，末尾追加）
add_executable(push_all_rules_tests
    test_push_all_rules.cpp
    ${CPP_DIR}/plugins/push_all/rules.cpp
)
target_include_directories(push_all_rules_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(push_all_rules_tests PRIVATE cxx_std_17)
set_target_properties(push_all_rules_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME push_all_rules_tests COMMAND push_all_rules_tests)

# P4 阶段 A2a：infra/scheduler 形式化 host 测试（末尾追加）
add_executable(infra_scheduler_tests
    test_infra_scheduler.cpp
    ${CPP_DIR}/infra/scheduler.cpp
    ${CPP_DIR}/infra/runtime.cpp
)
target_include_directories(infra_scheduler_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(infra_scheduler_tests PRIVATE cxx_std_17)
set_target_properties(infra_scheduler_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(infra_scheduler_tests PRIVATE Threads::Threads)
add_test(NAME infra_scheduler_tests COMMAND infra_scheduler_tests)

# P4 阶段 B1：special_equip 纯逻辑 host 测试（跳过 bit4 重放判定 / 出售门禁决策，末尾追加）
add_executable(special_equip_rules_tests
    test_special_equip_rules.cpp
    ${CPP_DIR}/plugins/special_equip/rules.cpp
)
target_include_directories(special_equip_rules_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(special_equip_rules_tests PRIVATE cxx_std_17)
set_target_properties(special_equip_rules_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME special_equip_rules_tests COMMAND special_equip_rules_tests)

# W1：商店扩展物品出售计数/计价纯函数 host 测试（header-only，末尾追加）
add_executable(store_sell_rules_tests
    test_store_sell_rules.cpp
)
target_include_directories(store_sell_rules_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(store_sell_rules_tests PRIVATE cxx_std_17)
add_test(NAME store_sell_rules_tests COMMAND store_sell_rules_tests)

# B4-1：infra/store MSAV 容器层 host 测试（末尾追加）
add_executable(infra_store_tests
    test_infra_store.cpp
    ${CPP_DIR}/infra/store.cpp
)
target_include_directories(infra_store_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(infra_store_tests PRIVATE cxx_std_17)
add_test(NAME infra_store_tests COMMAND infra_store_tests)

# D1-b0：面板注册表纯数据面 host 测试（末尾追加）
add_executable(infra_panels_tests
    test_infra_panels.cpp
    ${CPP_DIR}/infra/panels.cpp
)
target_include_directories(infra_panels_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(infra_panels_tests PRIVATE cxx_std_17)
set_target_properties(infra_panels_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME infra_panels_tests COMMAND infra_panels_tests)

# D1-b2：core.ui 面板栈 + 分页规则 host 测试（fake provider；末尾追加）
add_executable(core_ui_panels_tests
    test_core_ui_panels.cpp
    ${CPP_DIR}/plugins/core.custom_ui/panels/stack.cpp
)
target_include_directories(core_ui_panels_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(core_ui_panels_tests PRIVATE cxx_std_17)
set_target_properties(core_ui_panels_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME core_ui_panels_tests COMMAND core_ui_panels_tests)

# D1-b3：autosell 插件清单/资源 host 测试（声明式 claim + provider 登记；末尾追加）
add_executable(autosell_plugin_tests
    test_autosell_plugin.cpp
    ${CPP_DIR}/plugins/autosell/plugin.cpp
    ${CPP_DIR}/infra/panels.cpp
)
target_include_directories(autosell_plugin_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(autosell_plugin_tests PRIVATE cxx_std_17)
set_target_properties(autosell_plugin_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME autosell_plugin_tests COMMAND autosell_plugin_tests)

# ADR 0001 D3/D4：character_panel_i18n 插件清单/资源 host 测试（角色面板作用域 hook claim +
# 标签表连续性与占位 + backup 交付）。文本机制归 core.custom_ui（其清单见 native_choice_tests）。
# 显式锚定 C++17：contracts/plugin.h 字段名 requires 在 C++20 是关键字。
add_executable(character_panel_i18n_tests
    test_character_panel_i18n.cpp
    ${CPP_DIR}/plugins/character_panel_i18n/plugin.cpp
    ${CPP_DIR}/plugins/character_panel_i18n/feature.cpp
    ${CPP_DIR}/plugins/character_panel_i18n/charinfo_scope.cpp
    ${CPP_DIR}/hooks/dispatch.cpp
    ${CPP_DIR}/infra/log/qol_log.cpp
)
target_include_directories(character_panel_i18n_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
)
target_compile_features(character_panel_i18n_tests PRIVATE cxx_std_17)
set_target_properties(character_panel_i18n_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(character_panel_i18n_tests PRIVATE Threads::Threads)
add_test(NAME character_panel_i18n_tests COMMAND character_panel_i18n_tests)

# Quest scheduler 生命周期与撤销门禁 host 测试。
add_executable(quest_lifecycle_tests
    test_quest_lifecycle.cpp
    ${CPP_DIR}/plugins/core.quest/plugin.cpp
    ${CPP_DIR}/plugins/core.quest/feature.cpp
    ${CPP_DIR}/infra/scheduler.cpp
    ${CPP_DIR}/infra/runtime.cpp
    ${CPP_DIR}/infra/command_queue.cpp
    ${CPP_DIR}/infra/commands.cpp
    ${CPP_DIR}/infra/config.cpp
    ${CPP_DIR}/infra/events.cpp
    ${CPP_DIR}/infra/log/qol_log.cpp
)
target_include_directories(quest_lifecycle_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
)
target_compile_features(quest_lifecycle_tests PRIVATE cxx_std_17)
set_target_properties(quest_lifecycle_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(quest_lifecycle_tests PRIVATE Threads::Threads)
add_test(NAME quest_lifecycle_tests COMMAND quest_lifecycle_tests)

# attribute_range_display wrapper 撤销期透传 host 测试。
add_executable(attribute_range_display_tests
    test_attribute_range_display.cpp
    ${CPP_DIR}/plugins/attribute_range_display/feature.cpp
    ${CPP_DIR}/plugins/attribute_quality/rules.cpp
    ${CPP_DIR}/infra/log/qol_log.cpp
)
target_include_directories(attribute_range_display_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
)
target_compile_features(attribute_range_display_tests PRIVATE cxx_std_17)
set_target_properties(attribute_range_display_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(attribute_range_display_tests PRIVATE Threads::Threads)
add_test(NAME attribute_range_display_tests COMMAND attribute_range_display_tests)

# world_teleport 安装结果语义 host 测试。
add_executable(world_teleport_status_tests
    test_world_teleport_status.cpp
)
target_include_directories(world_teleport_status_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(world_teleport_status_tests PRIVATE cxx_std_17)
add_test(NAME world_teleport_status_tests COMMAND world_teleport_status_tests)

add_executable(world_teleport_install_tests
    test_world_teleport_install.cpp
    ${CPP_DIR}/plugins/world_teleport/plugin.cpp
)
target_include_directories(world_teleport_install_tests PRIVATE ${CPP_DIR})
target_compile_features(world_teleport_install_tests PRIVATE cxx_std_17)
set_target_properties(world_teleport_install_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME world_teleport_install_tests COMMAND world_teleport_install_tests)

# save_backup 回滚结构化结果 host 测试。
add_executable(save_backup_rollback_tests
    test_save_backup_rollback.cpp
)
target_include_directories(save_backup_rollback_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(save_backup_rollback_tests PRIVATE cxx_std_17)
add_test(NAME save_backup_rollback_tests COMMAND save_backup_rollback_tests)

# craft_ui 能力缺失时的 fail-closed 门禁纯逻辑 host 测试。
add_executable(craft_ui_guard_tests
    test_craft_ui_guard.cpp
)
target_include_directories(craft_ui_guard_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(craft_ui_guard_tests PRIVATE cxx_std_17)
add_test(NAME craft_ui_guard_tests COMMAND craft_ui_guard_tests)

# L2：Host SDK adapter 表转发与生命周期句柄 host 测试。
add_executable(host_adapter_tests
    test_host_adapters.cpp
    test_game_host_stubs.cpp
    ${CPP_DIR}/host/host_sdk.cpp
    ${CPP_DIR}/host/lifecycle_participants.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
    ${CPP_DIR}/host/game_host.cpp
    ${CPP_DIR}/game/adapter_ports/quest_port.cpp
    ${CPP_DIR}/game/adapter_ports/world_port.cpp
    ${CPP_DIR}/game/adapter_ports/item_port.cpp
    ${CPP_DIR}/game/access/shop.cpp
    ${CPP_DIR}/game/read/item_class.cpp
    ${CPP_DIR}/game/adapter_ports/character_port.cpp
    ${CPP_DIR}/game/adapter_ports/choice_port.cpp
    ${CPP_DIR}/game/adapter_ports/extension_bag_write_port.cpp
    ${CPP_DIR}/game/adapter_ports/game_host_port.cpp
    ${CPP_DIR}/game/adapter_ports/character_extras_port.cpp
    ${CPP_DIR}/game/adapter_ports/inventory_extras_port.cpp
    ${CPP_DIR}/game/adapter_ports/item_extras_port.cpp
    ${CPP_DIR}/game/adapter_ports/item_count_facade.cpp
    ${CPP_DIR}/game/adapter_ports/inventory_extension_facade.cpp
    ${CPP_DIR}/game/adapter_ports/save_port.cpp
    ${CPP_DIR}/game/adapter_ports/save_call_port.cpp
    ${CPP_DIR}/game/adapter_ports/craft_port.cpp
    ${CPP_DIR}/game/feature/game_feature.cpp
    ${CPP_DIR}/infra/command_queue.cpp
    ${CPP_DIR}/infra/commands.cpp
    ${CPP_DIR}/infra/config.cpp
    ${CPP_DIR}/infra/events.cpp
    ${CPP_DIR}/infra/capabilities.cpp
    ${CPP_DIR}/infra/runtime.cpp
    ${CPP_DIR}/infra/scheduler.cpp
    ${CPP_DIR}/infra/store.cpp
    ${CPP_DIR}/infra/panels.cpp
    ${CPP_DIR}/hooks/dispatch.cpp
    ${CPP_DIR}/hooks/popup_state.cpp
    ${CPP_DIR}/hooks/registry.cpp
    ${CPP_DIR}/game/access/popup_state.cpp
    ${CPP_DIR}/game/access/symbols.cpp
    ${CPP_DIR}/game/symbols/symbol_resolver.cpp
    ${CPP_DIR}/infra/popup_enter_alias.cpp
    ${CPP_DIR}/infra/log/qol_log.cpp
)
target_include_directories(host_adapter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
    ${CPP_DIR}/game/read
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(host_adapter_tests PRIVATE cxx_std_17)
set_target_properties(host_adapter_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(host_adapter_tests PRIVATE Threads::Threads)
add_test(NAME host_adapter_tests COMMAND host_adapter_tests)

# P5-1：core.api HTTP 路由登记、重复拒绝、caller-owned list 与 lease 归零检查。
add_executable(http_registry_tests
    test_http_registry.cpp
    ${CPP_DIR}/host/http_registry.cpp
)
target_include_directories(http_registry_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(http_registry_tests PRIVATE cxx_std_17)
set_target_properties(http_registry_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME http_registry_tests COMMAND http_registry_tests)

# P5-4-0：HTTP 模板 dispatch、query handler、owner 注销等待与 inactive 错误语义。
add_executable(http_dispatch_tests
    test_http_dispatch.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
)
target_include_directories(http_dispatch_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(http_dispatch_tests PRIVATE cxx_std_17)
set_target_properties(http_dispatch_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(http_dispatch_tests PRIVATE Threads::Threads)
add_test(NAME http_dispatch_tests COMMAND http_dispatch_tests)

# 批 5-0：/api/op/features/lifecycle owner command handler 响应形状（成功/失败/未知动作/投递失败）。
add_executable(core_api_lifecycle_tests
    test_core_api_lifecycle.cpp
    ${CPP_DIR}/plugins/core.api/plugin.cpp
    ${CPP_DIR}/plugins/core.api/json_field.cpp
)
target_include_directories(core_api_lifecycle_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(core_api_lifecycle_tests PRIVATE cxx_std_17)
set_target_properties(core_api_lifecycle_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME core_api_lifecycle_tests COMMAND core_api_lifecycle_tests)

# U11-1：UI contract ABI + host ui-kit adapter 转发/错误语义测试。
add_executable(ui_adapter_tests
    test_ui_adapter.cpp
    ${CPP_DIR}/host/ui_adapter.cpp
)
target_include_directories(ui_adapter_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(ui_adapter_tests PRIVATE cxx_std_17)
set_target_properties(ui_adapter_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME ui_adapter_tests COMMAND ui_adapter_tests)

# U11-2：core.ui 标准组件创建/更新/命中/销毁与错误语义 host 测试。
add_executable(core_ui_components_tests
    test_core_ui_components.cpp
    ${CPP_DIR}/plugins/core.custom_ui/ui_components.cpp
)
target_include_directories(core_ui_components_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(core_ui_components_tests PRIVATE cxx_std_17)
set_target_properties(core_ui_components_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME core_ui_components_tests COMMAND core_ui_components_tests)

# P1-A：item_count facade provider 槽、未驻留错误与空参数契约。
# ITM-13：中立物品域扩展 provider facade（set-once / resident-available 分离 / 缺成员拒绝）。
add_executable(inventory_extension_facade_tests
    test_inventory_extension_facade.cpp
    ${CPP_DIR}/host/inventory_extension_facade.cpp
    ${CPP_DIR}/game/adapter_ports/inventory_extension_facade.cpp
)
target_include_directories(inventory_extension_facade_tests PRIVATE ${CPP_DIR})
target_compile_features(inventory_extension_facade_tests PRIVATE cxx_std_17)
set_target_properties(inventory_extension_facade_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME inventory_extension_facade_tests COMMAND inventory_extension_facade_tests)

add_executable(item_count_facade_tests
    test_item_count_facade.cpp
    ${CPP_DIR}/host/item_count_facade.cpp
    ${CPP_DIR}/game/adapter_ports/item_count_facade.cpp
)
target_include_directories(item_count_facade_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(item_count_facade_tests PRIVATE cxx_std_17)
set_target_properties(item_count_facade_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME item_count_facade_tests COMMAND item_count_facade_tests)

# ITM-13：同步存档/UI 生命周期参与者注册与有序分派。
add_executable(lifecycle_participant_tests
    test_lifecycle_participants.cpp
    ${CPP_DIR}/host/lifecycle_participants.cpp
    ${CPP_DIR}/game/adapter_ports/lifecycle_participant_facade.cpp
)
target_include_directories(lifecycle_participant_tests PRIVATE ${CPP_DIR})
target_compile_features(lifecycle_participant_tests PRIVATE cxx_std_17)
set_target_properties(lifecycle_participant_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(lifecycle_participant_tests PRIVATE Threads::Threads)
add_test(NAME lifecycle_participant_tests COMMAND lifecycle_participant_tests)

# P1-B1：stack_limit 999 原子组 claim 清单、provider 驻留/注销与未知类别 fail-closed。
add_executable(stack_limit_plugin_tests
    test_stack_limit_plugin.cpp
    ${CPP_DIR}/plugins/stack_limit/plugin.cpp
    ${CPP_DIR}/host/item_count_facade.cpp
    ${CPP_DIR}/game/adapter_ports/item_count_facade.cpp
    ${CPP_DIR}/game/access/patches.cpp
    ${CPP_DIR}/game/read/item_class.cpp
)
target_include_directories(stack_limit_plugin_tests PRIVATE
    ${CPP_DIR}
    ${CPP_DIR}/game/read
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(stack_limit_plugin_tests PRIVATE cxx_std_17)
set_target_properties(stack_limit_plugin_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME stack_limit_plugin_tests COMMAND stack_limit_plugin_tests)

add_executable(world_facts_plugin_tests
    test_world_facts_plugin.cpp
    ${CPP_DIR}/plugins/core.world/plugin.cpp
)
target_include_directories(world_facts_plugin_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(world_facts_plugin_tests PRIVATE cxx_std_17)
set_target_properties(world_facts_plugin_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME world_facts_plugin_tests COMMAND world_facts_plugin_tests)

# P5-4-1：world_facts 7 条 /api/world/map/* query handler 的注册/快照/注销接缝。
add_executable(world_facts_dispatch_tests
    test_world_facts_dispatch.cpp
    ${CPP_DIR}/plugins/core.world/plugin.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
)
target_include_directories(world_facts_dispatch_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(world_facts_dispatch_tests PRIVATE cxx_std_17)
set_target_properties(world_facts_dispatch_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(world_facts_dispatch_tests PRIVATE Threads::Threads)
add_test(NAME world_facts_dispatch_tests COMMAND world_facts_dispatch_tests)

# contracts 层共享的无状态 BFS 纯函数 host 覆盖。
add_executable(nav_bfs_tests test_nav_bfs.cpp)
target_include_directories(nav_bfs_tests PRIVATE ${CPP_DIR})
target_compile_features(nav_bfs_tests PRIVATE cxx_std_17)
set_target_properties(nav_bfs_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME nav_bfs_tests COMMAND nav_bfs_tests)

# T2l-w-read：world 读 8 函数 host 内联（host/legacy_json_builders.h）与 api/native 原文
# frozen 复刻逐字节双跑（fake 环境：hero/瓦片/pool/charloc/drops/nav）。
add_executable(world_legacy_readers_tests
    test_world_legacy_readers.cpp
    ${CPP_DIR}/game/access/world_read.cpp
)
target_include_directories(world_legacy_readers_tests PRIVATE
    ${CPP_DIR}
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(world_legacy_readers_tests PRIVATE cxx_std_17)
set_target_properties(world_legacy_readers_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME world_legacy_readers_tests COMMAND world_legacy_readers_tests)

# T2l-rh：contracts/runtime_hooks.h 契约面（POD/版本/字段签名/帧点位常量）。
add_executable(runtime_hooks_contract_tests
    test_runtime_hooks_contract.cpp
)
target_include_directories(runtime_hooks_contract_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(runtime_hooks_contract_tests PRIVATE cxx_std_17)
set_target_properties(runtime_hooks_contract_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME runtime_hooks_contract_tests COMMAND runtime_hooks_contract_tests)

# T2l-w-write：game 层日志缝（qol_log_seam.h）——静默降级、输出序数/正文、帧节流路径。
add_executable(qol_log_seam_tests
    test_qol_log_seam.cpp
)
target_include_directories(qol_log_seam_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(qol_log_seam_tests PRIVATE cxx_std_17)
set_target_properties(qol_log_seam_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME qol_log_seam_tests COMMAND qol_log_seam_tests)

add_executable(character_facts_plugin_tests
    test_character_facts_plugin.cpp
    ${CPP_DIR}/plugins/core.character/plugin.cpp
    ${CPP_DIR}/plugins/core.character/json_slice.cpp
    ${CPP_DIR}/infra/capabilities.cpp
    ${CPP_DIR}/game/ops/character_write.cpp
    ${CPP_DIR}/game/ops/character_state_read.cpp
    ${CPP_DIR}/game/ops/game_ops_common.cpp
)
target_include_directories(character_facts_plugin_tests PRIVATE
    ${CPP_DIR}
    ${CPP_DIR}/game/read
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(character_facts_plugin_tests PRIVATE cxx_std_17)
target_compile_options(character_facts_plugin_tests PRIVATE -ffunction-sections -fdata-sections)
target_link_options(character_facts_plugin_tests PRIVATE -Wl,--gc-sections)
set_target_properties(character_facts_plugin_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
# T2h-B1：W1 非主线程 smoke 需要 std::thread。
target_link_libraries(character_facts_plugin_tests PRIVATE Threads::Threads)
add_test(NAME character_facts_plugin_tests COMMAND character_facts_plugin_tests)

# P5-4-2：item_facts 6 条 /api/item/* GET query handler 的注册/快照/富化/注销接缝。
add_executable(item_facts_dispatch_tests
    test_item_facts_dispatch.cpp
    ${CPP_DIR}/infra/command_queue.cpp
    ${CPP_DIR}/plugins/core.item/plugin.cpp
    ${CPP_DIR}/plugins/core.item/json_lite.cpp
    ${CPP_DIR}/plugins/core.item/response.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
    # T2h-B0：legacy 读实现入 host golden（fake 环境）
    ${CPP_DIR}/game/access/inventory_legacy_read.cpp
    ${CPP_DIR}/game/adapter_ports/inventory_extension_facade.cpp
    ${CPP_DIR}/game/ops/game_ops_common.cpp
    # T2h-B3#2：debug raw 整函数搬迁后的新落点
    ${CPP_DIR}/game/read/item_class.cpp
)
target_include_directories(item_facts_dispatch_tests PRIVATE
    ${CPP_DIR}
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(item_facts_dispatch_tests PRIVATE cxx_std_17)
set_target_properties(item_facts_dispatch_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(item_facts_dispatch_tests PRIVATE Threads::Threads)
add_test(NAME item_facts_dispatch_tests COMMAND item_facts_dispatch_tests)

# P5-4-2：quest 4 条 /api/quest* GET query handler 的注册/快照/片段拼接/注销接缝。
add_executable(quest_dispatch_tests
    test_quest_dispatch.cpp
    ${CPP_DIR}/plugins/core.quest/plugin.cpp
    ${CPP_DIR}/plugins/core.quest/feature.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
)
target_include_directories(quest_dispatch_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(quest_dispatch_tests PRIVATE cxx_std_17)
set_target_properties(quest_dispatch_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(quest_dispatch_tests PRIVATE Threads::Threads)
add_test(NAME quest_dispatch_tests COMMAND quest_dispatch_tests)

# P5-4-4：character_facts 15 条 /api/character/* GET query handler 的注册/快照/组合/404/
# 非 world 降级/注销接缝。
add_executable(character_facts_dispatch_tests
    test_character_facts_dispatch.cpp
    ${CPP_DIR}/plugins/core.character/plugin.cpp
    ${CPP_DIR}/plugins/core.character/json_slice.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
)
target_include_directories(character_facts_dispatch_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(character_facts_dispatch_tests PRIVATE cxx_std_17)
set_target_properties(character_facts_dispatch_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(character_facts_dispatch_tests PRIVATE Threads::Threads)
add_test(NAME character_facts_dispatch_tests COMMAND character_facts_dispatch_tests)

# 临时 core.feature 对唯一已证实未实现路由的 501 owner handler。
add_executable(core_feature_plugin_tests
    test_core_feature_plugin.cpp
    ${CPP_DIR}/plugins/core.feature/plugin.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
)
target_include_directories(core_feature_plugin_tests PRIVATE ${CPP_DIR})
target_compile_features(core_feature_plugin_tests PRIVATE cxx_std_17)
set_target_properties(core_feature_plugin_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(core_feature_plugin_tests PRIVATE Threads::Threads)
add_test(NAME core_feature_plugin_tests COMMAND core_feature_plugin_tests)

# D1 批 1：静态资产 ingress、12 route owner responses 和动态大响应缓冲。
add_executable(core_data_plugin_tests
    test_core_data_plugin.cpp
    ${CPP_DIR}/plugins/core.data/plugin.cpp
    ${CPP_DIR}/plugins/core.data/json.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
)
target_include_directories(core_data_plugin_tests PRIVATE ${CPP_DIR})
target_compile_features(core_data_plugin_tests PRIVATE cxx_std_17)
set_target_properties(core_data_plugin_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(core_data_plugin_tests PRIVATE Threads::Threads)
add_test(NAME core_data_plugin_tests COMMAND core_data_plugin_tests)

# P5-4-3：system_facts 4 条 /api/system/{snapshot,game,info,game_frame} query handler 的
# 注册/快照/身份降级/注销接缝。
add_executable(system_facts_dispatch_tests
    test_system_facts_dispatch.cpp
    ${CPP_DIR}/plugins/core.system/plugin.cpp
    ${CPP_DIR}/host/http_registry.cpp
    ${CPP_DIR}/host/http_dispatch.cpp
)
target_include_directories(system_facts_dispatch_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(system_facts_dispatch_tests PRIVATE cxx_std_17)
set_target_properties(system_facts_dispatch_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(system_facts_dispatch_tests PRIVATE Threads::Threads)
add_test(NAME system_facts_dispatch_tests COMMAND system_facts_dispatch_tests)

# T4': core.settings config/list 序列化字段顺序、类型与 JSON 字符串转义。
add_executable(core_settings_config_tests
    test_core_settings_config.cpp
    ${CPP_DIR}/plugins/core.settings/plugin.cpp
    ${CPP_DIR}/infra/config.cpp
    ${CPP_DIR}/infra/events.cpp
)
target_include_directories(core_settings_config_tests PRIVATE ${CPP_DIR})
target_compile_features(core_settings_config_tests PRIVATE cxx_std_17)
set_target_properties(core_settings_config_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME core_settings_config_tests COMMAND core_settings_config_tests)

# P1-B1：core_patches 仅保留 host Hive claim；12 条 S2 claim 不得回流。
add_executable(core_patches_shape_tests test_core_patches_shape.cpp)
target_compile_definitions(core_patches_shape_tests PRIVATE
    QOL_CORE_PATCHES_SOURCE="${CPP_DIR}/host/core_patches.cpp"
)
target_compile_features(core_patches_shape_tests PRIVATE cxx_std_17)
set_target_properties(core_patches_shape_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME core_patches_shape_tests COMMAND core_patches_shape_tests)

# core.ui_state wipeout 对话动作必须绕过通用 popup 按钮处理。
add_executable(ui_state_dialog_tests
    test_ui_state_dialog.cpp
    ${CPP_DIR}/game/access/ui_state.cpp
    ${CPP_DIR}/game/adapter_ports/lifecycle_participant_facade.cpp
)
target_include_directories(ui_state_dialog_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
    ${CPP_DIR}/game/read
    ${CPP_DIR}/game/symbols
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(ui_state_dialog_tests PRIVATE cxx_std_17)
set_target_properties(ui_state_dialog_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(ui_state_dialog_tests PRIVATE -ffunction-sections -fdata-sections)
target_link_options(ui_state_dialog_tests PRIVATE -Wl,--gc-sections)
add_test(NAME ui_state_dialog_tests COMMAND ui_state_dialog_tests)
