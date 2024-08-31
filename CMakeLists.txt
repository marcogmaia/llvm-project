cmake_minimum_required(VERSION 3.20.0)
project(llvm-clangd)



# set(BUILD_SHARED_LIBS ON)
set(LLVM_ENABLE_PROJECTS "clang;clang-tools-extra" CACHE STRING "")
set(LLVM_TARGETS_TO_BUILD "X86" CACHE STRING "")
set(LLVM_ENABLE_PCH ON CACHE BOOL "")
set(LLVM_ENABLE_LLD ON CACHE BOOL "")
set(LLVM_PARALLEL_LINK_JOBS "1" CACHE STRING "")
# set(LLVM_HOST_TRIPLE "x86_64-pc-win32" CACHE STRING "")

# Adjust this path to your LLVM source directory
set(LLVM_SOURCE_DIR "${CMAKE_SOURCE_DIR}/llvm")

# add_compile_options("-Wno-language-extension-token")

# add_link_options("/INCREMENTAL:NO")

add_subdirectory("${LLVM_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/llvm")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
