set(LLVM_LINK_COMPONENTS support)

add_clang_executable(omp-detect
	OMPDetect.cpp
  )

target_link_libraries(omp-detect
  PRIVATE
  clangAST
  clangASTMatchers
  clangBasic
  clangFrontend
  clangSerialization
  clangTooling
  )

