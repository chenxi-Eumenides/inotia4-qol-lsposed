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
    ${CPP_DIR}/core/native/game_json.cpp
    ${CPP_DIR}/core/native/game_nav.cpp
    ${CPP_DIR}/api/native/game_save_preflight.cpp
    ${CPP_DIR}/feature/save_backup/save_backup_bundle.cpp
    ${CPP_DIR}/feature/world_teleport/world_teleport_rules.cpp
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
    ${CPP_DIR}/feature/attribute_range/attribute_range.cpp
)
target_include_directories(attribute_range_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(attribute_range_tests PRIVATE cxx_std_17)
add_test(NAME attribute_range_tests COMMAND attribute_range_tests)

add_executable(autosell_rules_tests
    test_autosell_rules.cpp
    ${CPP_DIR}/feature/autosell/autosell_rules.cpp
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

# 合成器宝石合成优化纯逻辑 host 测试（gem-craft-optimization 阶段1，末尾追加）
add_executable(gemcraft_rules_tests
    test_gemcraft_rules.cpp
    ${CPP_DIR}/feature/gemcraft/gemcraft_rules.cpp
)
target_include_directories(gemcraft_rules_tests PRIVATE
    ${CPP_DIR}
)
target_compile_features(gemcraft_rules_tests PRIVATE cxx_std_17)
add_test(NAME gemcraft_rules_tests COMMAND gemcraft_rules_tests)

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
