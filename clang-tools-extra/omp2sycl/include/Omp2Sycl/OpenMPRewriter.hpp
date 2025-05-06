#pragma once
#include "Omp2Sycl/Rewriters.hpp"

namespace clang {
class ASTContext;
class OMPTargetParallelForDirective;
class OMPTargetDataDirective;
}

namespace Omp2Sycl {

/// 把 OpenMP 指令改写成 SYCL
class OpenMPRewriter final : public IRewriter {
public:
  explicit OpenMPRewriter(clang::Rewriter &R);

  void run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

private:
  void rewriteParallelFor(const clang::OMPTargetParallelForDirective *,
                          clang::ASTContext &);
  void rewriteTargetData(const clang::OMPTargetDataDirective *,
                         clang::ASTContext &);

  void recursiveRewrite(const clang::Stmt *, clang::ASTContext &);

  clang::Rewriter &TheRewriter;
};

} // namespace Omp2Sycl
