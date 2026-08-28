cmake_minimum_required(VERSION 3.25)
project(cxx_dead_target_fixture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(objects OBJECT objects.cpp)
add_library(core STATIC core.cpp)
target_sources(core PRIVATE $<TARGET_OBJECTS:objects>)

add_library(shared SHARED shared.cpp)
target_sources(shared PUBLIC FILE_SET HEADERS FILES shared.hpp)
target_compile_options(shared PRIVATE -fvisibility=hidden)

add_library(header_only INTERFACE)
target_sources(header_only INTERFACE FILE_SET HEADERS FILES header_only.hpp)

add_executable(production_app production.cpp)
target_link_libraries(production_app PRIVATE core shared)

add_executable(test_app test.cpp)
target_link_libraries(test_app PRIVATE core)
