cmake_minimum_required(VERSION 3.15)
project(FrostbiteGen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Frostbite 3 games are 64-bit; a 32-bit DLL cannot be injected into them.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "FrostbiteGen must be built for x64 (configure with: cmake -B build -A x64)")
endif()

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
    # /EHa: run C++ destructors when an SEH exception (e.g. an access violation caught by
    # __try/__except) unwinds the stack, so a skipped type does not leak open files.
    target_compile_options(FrostbiteGen PRIVATE /EHa /W3)
endif()

# Output the DLL with a clean name
set_target_properties(FrostbiteGen PROPERTIES
    OUTPUT_NAME "FrostbiteGen"
    SUFFIX ".dll"
)
