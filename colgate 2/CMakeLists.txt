cmake_minimum_required(VERSION 3.22)

# Must be set before project() so they reach the compiler checks.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Oldest macOS the plug-in supports")

project(Colgate VERSION 1.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(COLGATE_BUILD_PLUGIN "Build the AU and VST3 plug-ins (downloads JUCE)" ON)
option(COLGATE_BUILD_TESTS "Build the engine test suite" ON)

add_subdirectory(source/engine)

if(COLGATE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(COLGATE_BUILD_PLUGIN)
    add_subdirectory(source/plugin)
endif()
