cmake_minimum_required(VERSION 3.16)
project(RoundsCore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(rounds_core STATIC
  src/engine.cpp
  src/footswitch.cpp
  src/looper.cpp
  src/rounds_core.cpp
)
target_include_directories(rounds_core PUBLIC include src)
target_compile_options(rounds_core PRIVATE -Wall -Wextra)

enable_testing()
add_executable(rounds_tests tests/test_main.cpp)
target_link_libraries(rounds_tests rounds_core)
add_test(NAME rounds_tests COMMAND rounds_tests)
