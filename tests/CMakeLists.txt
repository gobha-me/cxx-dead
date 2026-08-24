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
