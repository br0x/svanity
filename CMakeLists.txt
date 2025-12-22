cmake_minimum_required(VERSION 3.13)
project(svanity C)

set(CMAKE_C_STANDARD 11)

# Find required packages
find_package(OpenCL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)

# Read OpenCL kernel and generate header
file(READ "${CMAKE_SOURCE_DIR}/src/opencl/entry.cl" OPENCL_KERNEL_SOURCE)
# Escape quotes and backslashes for C string
string(REPLACE "\\" "\\\\" OPENCL_KERNEL_SOURCE "${OPENCL_KERNEL_SOURCE}")
string(REPLACE "\"" "\\\"" OPENCL_KERNEL_SOURCE "${OPENCL_KERNEL_SOURCE}")
string(REPLACE "\n" "\\n\"\n\"" OPENCL_KERNEL_SOURCE "${OPENCL_KERNEL_SOURCE}")

# Generate header file with embedded kernel
file(WRITE "${CMAKE_BINARY_DIR}/opencl_kernel.h"
"#ifndef OPENCL_KERNEL_H
#define OPENCL_KERNEL_H

const char* opencl_kernel_source = \"${OPENCL_KERNEL_SOURCE}\";

#endif
")

# Include directories
include_directories(${OpenCL_INCLUDE_DIRS})
include_directories(${SODIUM_INCLUDE_DIRS})
include_directories(${CMAKE_BINARY_DIR})

# Source files
set(SOURCES
    src/main.c
    src/base58.c
    src/argtable3.c
    src/solana.c
    src/gpu.c
    src/vanity.c
)

# Create executable
add_executable(svanity ${SOURCES})

# Link libraries
target_link_libraries(svanity
    ${OpenCL_LIBRARIES}
    ${SODIUM_LIBRARIES}
    pthread
    m
)

# Compiler flags
target_compile_options(svanity PRIVATE -Wall -O3)
