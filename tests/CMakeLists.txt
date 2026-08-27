add_executable(cxx_dead_process_fixture process_fixture.cpp)
target_compile_features(cxx_dead_process_fixture PRIVATE cxx_std_23)

add_executable(cxx_dead_tests unit_tests.cpp)
target_link_libraries(cxx_dead_tests PRIVATE cxx_dead_core)
target_compile_features(cxx_dead_tests PRIVATE cxx_std_23)
target_compile_definitions(cxx_dead_tests PRIVATE
    CXX_DEAD_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/application"
    CXX_DEAD_CONSTRUCTION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/construction"
    CXX_DEAD_CALLABLE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callables"
    CXX_DEAD_PROVIDER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/providers"
    CXX_DEAD_PROCESS_FIXTURE="$<TARGET_FILE:cxx_dead_process_fixture>"
)

add_test(NAME cxx_dead_tests COMMAND cxx_dead_tests)
set_tests_properties(cxx_dead_tests PROPERTIES TIMEOUT 20)

add_executable(cxx_dead_golden_tests golden_tests.cpp)
target_link_libraries(cxx_dead_golden_tests PRIVATE cxx_dead_core)
target_compile_features(cxx_dead_golden_tests PRIVATE cxx_std_23)
target_compile_definitions(cxx_dead_golden_tests PRIVATE
    CXX_DEAD_GOLDEN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden"
    CXX_DEAD_PROCESS_FIXTURE="$<TARGET_FILE:cxx_dead_process_fixture>"
)

add_test(NAME cxx_dead_golden_tests COMMAND cxx_dead_golden_tests)
set_tests_properties(cxx_dead_golden_tests PROPERTIES TIMEOUT 10)

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
        CXX_DEAD_CONSTRUCTION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/construction"
        CXX_DEAD_CALLABLE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callables"
        CXX_DEAD_PROVIDER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/providers"
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

add_test(
    NAME cxx_dead_callable_cli
    COMMAND $<TARGET_FILE:cxx-dead>
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callables/compile_commands.json"
        --project-root "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/callables"
        --ast-filter callable_fixture
        --root callable_fixture::run
        --callback-registration callable_fixture::registrar:0
        --callback-registration callable_fixture::member_registrar:0
        --format json
)
set_tests_properties(cxx_dead_callable_cli PROPERTIES
    PASS_REGULAR_EXPRESSION "\"provider_reachable_symbols\": 2"
)

add_test(
    NAME cxx_dead_invalid_callback_cli
    COMMAND $<TARGET_FILE:cxx-dead> --callback-registration invalid
)
set_tests_properties(cxx_dead_invalid_callback_cli PROPERTIES
    WILL_FAIL TRUE
)

add_test(
    NAME cxx_dead_provider_cli
    COMMAND $<TARGET_FILE:cxx-dead>
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/providers/compile_commands.json"
        --project-root "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/providers"
        --ast-filter provider_fixture
        --provider-config "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/providers/provider.yaml"
        --format json
)
set_tests_properties(cxx_dead_provider_cli PROPERTIES
    PASS_REGULAR_EXPRESSION "\"suppressed_symbols\": 1"
)

add_test(
    NAME cxx_dead_provider_suppressed_policy_cli
    COMMAND $<TARGET_FILE:cxx-dead>
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/providers/compile_commands.json"
        --project-root "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/providers"
        --ast-filter provider_fixture
        --provider-config "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/providers/provider.yaml"
        --provider-config "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/providers/suppress-all.yaml"
        --fail-on-unreachable
        --format json
)
set_tests_properties(cxx_dead_provider_suppressed_policy_cli PROPERTIES
    PASS_REGULAR_EXPRESSION "\"actionable_unreachable_symbols\": 0"
)

add_test(
    NAME cxx_dead_invalid_provider_cli
    COMMAND $<TARGET_FILE:cxx-dead>
        --provider-config "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/providers/invalid-unknown.yaml"
)
set_tests_properties(cxx_dead_invalid_provider_cli PROPERTIES
    WILL_FAIL TRUE
)

add_test(
    NAME cxx_dead_incomplete_cli
    COMMAND "${CMAKE_COMMAND}"
        -DCXX_DEAD_EXECUTABLE=$<TARGET_FILE:cxx-dead>
        -DCXX_DEAD_GOLDEN_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden
        -DCXX_DEAD_FAILURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/incomplete-cli
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_failure_test.cmake
)
