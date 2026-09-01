cmake_minimum_required(VERSION 3.15)
project(FrostbiteGen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build as a DLL (injected into the game process)
add_library(FrostbiteGen SHARED
    main.cpp
    classinfo.cpp
    classinfo.h
    structs.h
    required.h
)

# MSVC-specific settings
if(MSVC)
    target_compile_definitions(FrostbiteGen PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
    # Enable SEH exceptions (__try/__except) — required for safe memory probing
    target_compile_options(FrostbiteGen PRIVATE /EHa)
endif()

# Output the DLL with a clean name
set_target_properties(FrostbiteGen PROPERTIES
    OUTPUT_NAME "FrostbiteGen"
    SUFFIX ".dll"
)
