# Compiles an SDK produced by tests/run_sdk_test.sh with MSVC (used by CI on Windows).
#   cmake -S tests/msvc_sdk_check -B build/msvc-sdk-check -A x64 -DSDK_DIR=<path to generated SDK>
cmake_minimum_required(VERSION 3.15)
project(FrostbiteGenSdkCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT SDK_DIR)
    message(FATAL_ERROR "Pass -DSDK_DIR=<generated SDK directory>")
endif()

add_executable(sdk_compile_check main.cpp)
target_include_directories(sdk_compile_check PRIVATE ${SDK_DIR})
target_compile_definitions(sdk_compile_check PRIVATE FBGEN_VERIFY_LAYOUT)
if(MSVC)
    target_compile_options(sdk_compile_check PRIVATE /EHa /W3)
endif()
