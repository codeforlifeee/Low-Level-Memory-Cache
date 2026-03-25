cmake_minimum_required(VERSION 3.20)
project(mini_redis_cache VERSION 1.0.0 LANGUAGES CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_TESTS "Build unit tests" ON)
option(ENABLE_BENCHMARKS "Build benchmark binary" ON)
option(ENABLE_CLI "Build Redis-like CLI simulator" ON)
option(ENABLE_HTTP "Build lightweight HTTP simulator" ON)
option(ENABLE_TCP "Build TCP command server" ON)

add_library(mini_redis_cache STATIC
    src/persistence.cpp
)
target_include_directories(mini_redis_cache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
    target_compile_options(mini_redis_cache PRIVATE /W4 /permissive-)
else()
    target_compile_options(mini_redis_cache PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(cache_demo src/main.cpp)
target_link_libraries(cache_demo PRIVATE mini_redis_cache)

if(ENABLE_CLI)
    add_executable(cache_cli src/cache_cli.cpp)
    target_link_libraries(cache_cli PRIVATE mini_redis_cache)
endif()

if(ENABLE_HTTP)
    include(FetchContent)
    FetchContent_Declare(
        cpp_httplib
        URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.17.3.zip
    )
    FetchContent_MakeAvailable(cpp_httplib)

    add_executable(cache_http src/cache_http.cpp)
    target_link_libraries(cache_http PRIVATE mini_redis_cache httplib::httplib)
endif()

if(ENABLE_TCP)
    add_executable(cache_tcp_server src/cache_tcp_server.cpp)
    target_link_libraries(cache_tcp_server PRIVATE mini_redis_cache)
    if(WIN32)
        target_link_libraries(cache_tcp_server PRIVATE ws2_32)
    endif()
endif()

if(ENABLE_BENCHMARKS)
    add_executable(cache_benchmark benchmarks/cache_benchmark_tuned.cpp)
    target_link_libraries(cache_benchmark PRIVATE mini_redis_cache)
endif()

if(ENABLE_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(cache_tests tests/cache_tests.cpp)
    target_link_libraries(cache_tests PRIVATE mini_redis_cache GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(cache_tests)
endif()
