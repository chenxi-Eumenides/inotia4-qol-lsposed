cmake_minimum_required(VERSION 3.16)
project(host_tests CXX)

# 独立 host 单测构建（不并入主 CMakeLists：主构建绑定 NDK 工具链，仅面向 arm64/arm）。
# 编译真实被测源文件（纯 STL 逻辑），以 stubs/ 覆盖 Android 专属头，ctest 运行不依赖设备。

enable_testing()

find_package(JNI REQUIRED)

set(CPP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

# 注：game_tiles.cpp 不在此列表——test_host.cpp 直接 #include 它，
# 以访问匿名命名空间的 base64_decode/parse_int_field（内部纯函数）。
add_executable(host_tests
    test_host.cpp
    test_stubs.cpp
    ${CPP_DIR}/feature/patch/inventory_find_item_poc.cpp
    ${CPP_DIR}/core/native/call_patch.cpp
    ${CPP_DIR}/core/native/qol_log.cpp
    ${CPP_DIR}/core/native/game_json.cpp
    ${CPP_DIR}/core/native/game_nav.cpp
    ${CPP_DIR}/api/native/game_save_preflight.cpp
    ${CPP_DIR}/features/save_backup/bundle.cpp
    ${CPP_DIR}/features/world_teleport/rules.cpp
)

target_include_directories(host_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
    ${CPP_DIR}/api/native
    ${CPP_DIR}/core/native
    ${CPP_DIR}/data/native
    ${JNI_INCLUDE_DIRS}
)

target_compile_features(host_tests PRIVATE cxx_std_17)

add_test(NAME host_tests COMMAND host_tests)

add_executable(stage4_hook_tests
    test_inventory_hook_stage4.cpp
    ${CPP_DIR}/feature/patch/inventory_hook_stage4.cpp
)
target_include_directories(stage4_hook_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(stage4_hook_tests PRIVATE cxx_std_17)
add_test(NAME stage4_hook_tests COMMAND stage4_hook_tests)

add_executable(attribute_range_tests
    test_attribute_range.cpp
    ${CPP_DIR}/domains/attribute_range/rules.cpp
)
target_include_directories(attribute_range_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(attribute_range_tests PRIVATE cxx_std_17)
add_test(NAME attribute_range_tests COMMAND attribute_range_tests)

# 自定义合成配方纯逻辑 host 测试（档位互逆 / 目录映射 / 表注入字节；末尾追加）
add_executable(custom_recipe_tests
    test_custom_recipe.cpp
    ${CPP_DIR}/domains/attribute_range/rules.cpp
    ${CPP_DIR}/feature/custom_recipe/custom_recipe_rules.cpp
    ${CPP_DIR}/feature/custom_recipe/custom_recipe_catalog.cpp
    ${CPP_DIR}/feature/custom_recipe/custom_recipe_table.cpp
)
target_include_directories(custom_recipe_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(custom_recipe_tests PRIVATE cxx_std_17)
add_test(NAME custom_recipe_tests COMMAND custom_recipe_tests)

add_executable(autosell_rules_tests
    test_autosell_rules.cpp
    ${CPP_DIR}/features/autosell/rules.cpp
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
    ${CPP_DIR}/core/native/qol_log.cpp
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
    ${CPP_DIR}/core/native/qol_log.cpp
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
    ${CPP_DIR}/core/native/game_feature.cpp
    ${CPP_DIR}/core/native/game_variant.cpp
    ${CPP_DIR}/core/native/qol_log.cpp
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
    ${CPP_DIR}/features/save_backup/bundle.cpp
)
target_include_directories(save_warehouse_validate_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(save_warehouse_validate_tests PRIVATE cxx_std_17)
add_test(NAME save_warehouse_validate_tests COMMAND save_warehouse_validate_tests)

# 原生选择框组件纯逻辑测试：spec 校验、串行化、关闭项和文本归属地址区间。
add_executable(native_choice_tests
    test_native_choice.cpp
)
target_include_directories(native_choice_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(native_choice_tests PRIVATE cxx_std_17)
add_test(NAME native_choice_tests COMMAND native_choice_tests)

# 角色属性面板标签中文化白名单 host 测试（纯表逻辑，header-inline，末尾追加）
add_executable(module_text_tests
    test_module_text.cpp
)
target_include_directories(module_text_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(module_text_tests PRIVATE cxx_std_17)
# 显式锚定 C++17：features/text_layer/feature.h 经 contracts/plugin.h 使用字段名 requires。
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
    ${CPP_DIR}/features/simple_mode/rules.cpp
)
target_include_directories(simple_mode_rules_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(simple_mode_rules_tests PRIVATE cxx_std_17)
add_test(NAME simple_mode_rules_tests COMMAND simple_mode_rules_tests)

# 模块 wrapper enter → 原版面板 VMA 反查注册表 host 测试（末尾追加）
add_executable(popup_enter_alias_tests
    test_popup_enter_alias.cpp
    ${CPP_DIR}/core/native/popup_enter_alias.cpp
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
    ${CPP_DIR}/host/plugin_registry.cpp
    ${CPP_DIR}/hooks/registry.cpp
    ${CPP_DIR}/hooks/install.cpp
    ${CPP_DIR}/hooks/transaction.cpp
    ${CPP_DIR}/game/access/patches.cpp
    ${CPP_DIR}/patch/claims.cpp
    ${CPP_DIR}/patch/instr_patch.cpp
    ${CPP_DIR}/infra/status.cpp
    ${CPP_DIR}/infra/capabilities.cpp
    ${CPP_DIR}/infra/config.cpp
    ${CPP_DIR}/infra/events.cpp
    ${CPP_DIR}/core/native/qol_log.cpp
)
target_include_directories(plugin_registry_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
    ${CPP_DIR}/data/native
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

# P3 Lane B：生命周期全路径 host 测试（真实 registry/tx/status/config + 假 tx 依赖，末尾追加）
# 显式锚定 C++17：contracts/plugin.h 字段名 requires 在 C++20 是关键字。
add_executable(plugin_lifecycle_tests
    test_plugin_lifecycle.cpp
    test_stubs.cpp
    ${CPP_DIR}/host/plugin_registry.cpp
    ${CPP_DIR}/hooks/registry.cpp
    ${CPP_DIR}/hooks/install.cpp
    ${CPP_DIR}/hooks/transaction.cpp
    ${CPP_DIR}/game/access/patches.cpp
    ${CPP_DIR}/patch/claims.cpp
    ${CPP_DIR}/patch/instr_patch.cpp
    ${CPP_DIR}/infra/status.cpp
    ${CPP_DIR}/infra/capabilities.cpp
    ${CPP_DIR}/infra/config.cpp
    ${CPP_DIR}/infra/events.cpp
    ${CPP_DIR}/core/native/qol_log.cpp
)
target_include_directories(plugin_lifecycle_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
    ${CPP_DIR}/data/native
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
    ${CPP_DIR}/host/plugin_registry.cpp
    ${CPP_DIR}/hooks/registry.cpp
    ${CPP_DIR}/hooks/install.cpp
    ${CPP_DIR}/hooks/transaction.cpp
    ${CPP_DIR}/game/access/patches.cpp
    ${CPP_DIR}/patch/claims.cpp
    ${CPP_DIR}/patch/instr_patch.cpp
    ${CPP_DIR}/infra/status.cpp
    ${CPP_DIR}/infra/capabilities.cpp
    ${CPP_DIR}/infra/config.cpp
    ${CPP_DIR}/infra/events.cpp
    ${CPP_DIR}/core/native/qol_log.cpp
)
target_include_directories(capability_lifecycle_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CPP_DIR}
    ${CPP_DIR}/data/native
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
    ${CPP_DIR}/game/access/patches.cpp
)
target_include_directories(patch_layer_tests PRIVATE
    ${CPP_DIR}
    ${CPP_DIR}/data/native
    ${JNI_INCLUDE_DIRS}
)
target_compile_features(patch_layer_tests PRIVATE cxx_std_17)
set_target_properties(patch_layer_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME patch_layer_tests COMMAND patch_layer_tests)

# P4 阶段 A1：push_all 纯逻辑 host 测试（party_decision 矩阵，末尾追加）
add_executable(push_all_rules_tests
    test_push_all_rules.cpp
    ${CPP_DIR}/features/push_all/rules.cpp
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
    ${CPP_DIR}/features/special_equip/rules.cpp
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
