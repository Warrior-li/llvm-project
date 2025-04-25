set(LLVM_LINK_COMPONENTS support)
find_package(OpenMP REQUIRED)

add_clang_executable(omp-detect
  main.cpp
	OMPDetect.cpp
  OMPDetect.h
  )

target_link_libraries(omp-detect
  PRIVATE
  clangAST
  clangASTMatchers
  clangBasic
  clangFrontend
  clangSerialization
  clangTooling
  OpenMP::OpenMP_CXX
  )

