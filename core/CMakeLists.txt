cmake_minimum_required(VERSION 3.25)
project(sorus_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
# Deps declare old cmake_minimum_required values; CMake 4 rejects < 3.5 and warns below 3.10.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_Declare(doctest URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(doctest json)

function(sorus_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()

file(GLOB SORUS_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(sorus_core STATIC ${SORUS_SOURCES})
target_include_directories(sorus_core PUBLIC include PRIVATE src)
target_link_libraries(sorus_core PRIVATE nlohmann_json::nlohmann_json)
sorus_warnings(sorus_core)

enable_testing()
include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)

file(GLOB SORUS_TESTS CONFIGURE_DEPENDS tests/test_*.cpp)
list(FILTER SORUS_TESTS EXCLUDE REGEX "test_no_alloc\.cpp$")
add_executable(sorus_tests tests/main.cpp ${SORUS_TESTS})
target_include_directories(sorus_tests PRIVATE src tests)
target_link_libraries(sorus_tests PRIVATE sorus_core doctest::doctest)
sorus_warnings(sorus_tests)
doctest_discover_tests(sorus_tests)

# Counts global operator new, so it gets its own binary and doctest main.
add_executable(sorus_no_alloc_test tests/test_no_alloc.cpp)
target_include_directories(sorus_no_alloc_test PRIVATE src tests)
target_link_libraries(sorus_no_alloc_test PRIVATE sorus_core doctest::doctest)
sorus_warnings(sorus_no_alloc_test)
doctest_discover_tests(sorus_no_alloc_test)

# doctest 2.4.x vs newer toolchains: its std forward declarations trip MSVC C5285, and under clang it always
# includes <ciso646>, which libstdc++ 15 #warns about in C++20.
foreach(t sorus_tests sorus_no_alloc_test)
  target_compile_definitions(${t} PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${t} PRIVATE "-Wno-#warnings")
  endif()
endforeach()

add_executable(sorus_bench bench/bench.cpp)
target_include_directories(sorus_bench PRIVATE src)
target_link_libraries(sorus_bench PRIVATE sorus_core)
sorus_warnings(sorus_bench)
