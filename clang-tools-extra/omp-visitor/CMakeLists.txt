set(LLVM_LINK_COMPONENTS support)

add_clang_executable(omp-visitor
  OMPVisitor.cpp
  )

target_link_libraries(omp-visitor
  PRIVATE
  clangAST
  clangASTMatchers
  clangBasic
  clangFrontend
  clangSerialization
  clangTooling
  )

