#pragma once
#include <memory>
#include <clang/ASTMatchers/ASTMatchFinder.h>

namespace clang { class Rewriter; }
namespace clang { class SourceManager; }

namespace Omp2Sycl {

/// 统一的 rewriter 接口，方便主程序按需扩展
class IRewriter : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  virtual ~IRewriter() = default;
};

/// 工厂函数 —— 返回实现实例
std::unique_ptr<IRewriter>
makeOpenMPRewriter(clang::Rewriter &R);

std::unique_ptr<IRewriter>
makeHeaderRewriter(clang::Rewriter &R, clang::SourceManager &SM);

} // namespace Omp2Sycl
