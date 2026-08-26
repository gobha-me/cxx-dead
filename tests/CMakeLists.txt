add_executable(cxx_dead_tests unit_tests.cpp)
target_link_libraries(cxx_dead_tests PRIVATE cxx_dead_core)
target_compile_features(cxx_dead_tests PRIVATE cxx_std_23)
target_compile_definitions(cxx_dead_tests PRIVATE
    CXX_DEAD_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/application"
)

add_test(NAME cxx_dead_tests COMMAND cxx_dead_tests)

add_executable(cxx_dead_golden_tests golden_tests.cpp)
target_link_libraries(cxx_dead_golden_tests PRIVATE cxx_dead_core)
target_compile_features(cxx_dead_golden_tests PRIVATE cxx_std_23)
target_compile_definitions(cxx_dead_golden_tests PRIVATE
    CXX_DEAD_GOLDEN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden"
)

add_test(NAME cxx_dead_golden_tests COMMAND cxx_dead_golden_tests)

add_executable(cxx_dead_scope_tests scope_tests.cpp)
target_link_libraries(cxx_dead_scope_tests PRIVATE cxx_dead_core)
target_compile_features(cxx_dead_scope_tests PRIVATE cxx_std_23)
target_compile_definitions(cxx_dead_scope_tests PRIVATE
    CXX_DEAD_SCOPE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scope"
)

add_test(NAME cxx_dead_scope_tests COMMAND cxx_dead_scope_tests)

set(TARGET_FIXTURE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/targets")
set(TARGET_FIXTURE_BUILD "${CMAKE_CURRENT_BINARY_DIR}/target-fixture-build")
file(MAKE_DIRECTORY "${TARGET_FIXTURE_BUILD}/.cmake/api/v1/query")
file(WRITE "${TARGET_FIXTURE_BUILD}/.cmake/api/v1/query/codemodel-v2" "")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${TARGET_FIXTURE_SOURCE}"
        -B "${TARGET_FIXTURE_BUILD}"
        -DCMAKE_BUILD_TYPE=Debug
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    RESULT_VARIABLE TARGET_FIXTURE_CONFIGURE_RESULT
    OUTPUT_VARIABLE TARGET_FIXTURE_CONFIGURE_OUTPUT
    ERROR_VARIABLE TARGET_FIXTURE_CONFIGURE_ERROR
)
if(NOT TARGET_FIXTURE_CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Could not configure target-aware fixture:\n"
        "${TARGET_FIXTURE_CONFIGURE_OUTPUT}\n${TARGET_FIXTURE_CONFIGURE_ERROR}"
    )
endif()
configure_file(
    "${TARGET_FIXTURE_SOURCE}/manifest.json.in"
    "${TARGET_FIXTURE_BUILD}/target-manifest.json"
    @ONLY
)

add_executable(cxx_dead_target_tests target_tests.cpp)
target_link_libraries(cxx_dead_target_tests PRIVATE cxx_dead_core)
target_compile_features(cxx_dead_target_tests PRIVATE cxx_std_23)
target_compile_definitions(cxx_dead_target_tests PRIVATE
    CXX_DEAD_TARGET_FIXTURE_SOURCE="${TARGET_FIXTURE_SOURCE}"
    CXX_DEAD_TARGET_FIXTURE_BUILD="${TARGET_FIXTURE_BUILD}"
)
add_test(NAME cxx_dead_target_tests COMMAND cxx_dead_target_tests)

add_test(
    NAME cxx_dead_target_cli
    COMMAND $<TARGET_FILE:cxx-dead>
        --cmake-build-dir "${TARGET_FIXTURE_BUILD}"
        --configuration Debug
        --target production_app
        --format json
)
set_tests_properties(cxx_dead_target_cli PROPERTIES
    PASS_REGULAR_EXPRESSION
        "\"mode\": \"target\".*\"target_name\": \"production_app\""
)

if(CXX_DEAD_ENABLE_LIBTOOLING)
    add_executable(cxx_dead_frontend_tests frontend_tests.cpp)
    target_link_libraries(cxx_dead_frontend_tests PRIVATE cxx_dead_core)
    target_compile_features(cxx_dead_frontend_tests PRIVATE cxx_std_23)
    target_compile_definitions(cxx_dead_frontend_tests PRIVATE
        CXX_DEAD_GOLDEN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden"
        CXX_DEAD_SCOPE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scope"
    )
    add_test(NAME cxx_dead_frontend_tests COMMAND cxx_dead_frontend_tests)
endif()
add_test(
    NAME cxx_dead_scope_cli
    COMMAND $<TARGET_FILE:cxx-dead>
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scope/compile_commands.json"
        --project-root "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scope"
        --report-path "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/scope/app"
        --format json
)
set_tests_properties(cxx_dead_scope_cli PROPERTIES
    PASS_REGULAR_EXPRESSION "\"indexed\": [1-9]"
)
