cmake_minimum_required(VERSION 3.25)

project(cxx-dead VERSION 0.16.0 LANGUAGES CXX)

option(CXX_DEAD_BUILD_TESTS "Build cxx-dead tests" ON)
option(CXX_DEAD_ENABLE_LIBTOOLING "Build the experimental LibTooling frontend" OFF)

find_package(yaml-cpp CONFIG REQUIRED)

add_library(cxx_dead_core
    src/artifact.cpp
    src/build_model.cpp
    src/cache.cpp
    src/compile_database.cpp
    src/differential.cpp
    src/graph.cpp
    src/indexer.cpp
    src/json.cpp
    src/process.cpp
    src/provider.cpp
    src/report.cpp
)

if(CXX_DEAD_ENABLE_LIBTOOLING)
    find_package(LLVM CONFIG REQUIRED)
    find_package(Clang CONFIG REQUIRED)
    target_sources(cxx_dead_core PRIVATE src/libtooling_indexer.cpp)
    target_include_directories(cxx_dead_core SYSTEM PRIVATE
        ${LLVM_INCLUDE_DIRS}
        ${CLANG_INCLUDE_DIRS}
    )
    target_link_libraries(cxx_dead_core PUBLIC clang-cpp LLVM)
else()
    target_sources(cxx_dead_core PRIVATE src/libtooling_stub.cpp)
endif()

target_include_directories(cxx_dead_core PUBLIC include)
target_link_libraries(cxx_dead_core PUBLIC yaml-cpp::yaml-cpp)
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
