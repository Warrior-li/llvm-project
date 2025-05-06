# omp2sycl/CMakeLists.txt  (in clang-tools-extra/omp2sycl)

cmake_minimum_required(VERSION 3.20)
project(omp2sycl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# ===== 库 =====
add_clang_library(OMP2SYCL
  src/Utils.cpp
  src/OpenMPRewriter.cpp
  src/HeaderRewriter.cpp

  LINK_LIBS
    clangAST
    clangASTMatchers
    clangBasic
    clangRewrite
    clangRewriteFrontend
    clangTooling
)

target_include_directories(OMP2SYCL
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ===== 可执行工具 =====
add_clang_tool(omp2sycl
  tools/omp2sycl.cpp
)
target_link_libraries(omp2sycl PRIVATE OMP2SYCL)
