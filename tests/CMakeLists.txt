# Host tests: cmake -S tests -B build/tests && cmake --build build/tests && build/tests/nano_tests
cmake_minimum_required(VERSION 3.16)
project(nano_tests CXX)
set(CMAKE_CXX_STANDARD 17)
add_subdirectory(../components/nano nano)
add_executable(nano_tests test_main.cpp)
target_link_libraries(nano_tests nano m)
target_compile_options(nano_tests PRIVATE -Wall -Wextra)
