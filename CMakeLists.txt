cmake_minimum_required(VERSION 3.20)
project(ZLCoro 
    VERSION 0.1.0 
    DESCRIPTION "High-performance C++20 coroutine framework"
    LANGUAGES CXX
)

# C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler options
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -fcoroutines)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0 -fsanitize=address -fsanitize=undefined)
        add_link_options(-fsanitize=address -fsanitize=undefined)
    else()
        add_compile_options(-O3 -DNDEBUG)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0 -fsanitize=address -fsanitize=undefined)
        add_link_options(-fsanitize=address -fsanitize=undefined)
    else()
        add_compile_options(-O3 -DNDEBUG)
    endif()
endif()

# Options
option(ZLCORO_BUILD_TESTS "Build tests" ON)
option(ZLCORO_BUILD_EXAMPLES "Build examples" ON)
option(ZLCORO_BUILD_BENCHMARKS "Build benchmarks" ON)
option(ZLCORO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ZLCORO_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# Find dependencies
find_package(Threads REQUIRED)

# Main library target (header-only)
add_library(zlcoro INTERFACE)
add_library(ZLCoro::zlcoro ALIAS zlcoro)

target_include_directories(zlcoro INTERFACE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(zlcoro INTERFACE cxx_std_20)
target_link_libraries(zlcoro INTERFACE Threads::Threads)

# Subdirectories
if(ZLCORO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(ZLCORO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ZLCORO_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Installation
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ 
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS zlcoro
    EXPORT ZLCoroTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT ZLCoroTargets
    FILE ZLCoroTargets.cmake
    NAMESPACE ZLCoro::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ZLCoro
)

# Generate config files (simplified for local development)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ZLCoroConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
