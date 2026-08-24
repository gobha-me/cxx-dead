cmake_minimum_required(VERSION 3.25)

project(cxx-dead VERSION 0.1.1 LANGUAGES CXX)

option(CXX_DEAD_BUILD_TESTS "Build cxx-dead tests" ON)

add_library(cxx_dead_core
    src/compile_database.cpp
    src/graph.cpp
    src/indexer.cpp
    src/json.cpp
    src/process.cpp
    src/report.cpp
)

target_include_directories(cxx_dead_core PUBLIC include)
target_compile_features(cxx_dead_core PUBLIC cxx_std_23)
target_compile_options(cxx_dead_core PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_executable(cxx-dead src/main.cpp)
target_link_libraries(cxx-dead PRIVATE cxx_dead_core)

include(GNUInstallDirs)
install(TARGETS cxx-dead RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(CXX_DEAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
