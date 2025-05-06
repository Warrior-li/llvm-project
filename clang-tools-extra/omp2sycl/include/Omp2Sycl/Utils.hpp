#pragma once
#include <clang/AST/AST.h>
#include <clang/Rewrite/Core/Rewriter.h>

namespace Omp2Sycl::detail {

/// 递归展开 CapturedStmt，返回最里层语句
inline const clang::Stmt *unwrapCaptured(const clang::Stmt *S) {
  while (const auto *Cap = llvm::dyn_cast_if_present<clang::CapturedStmt>(S))
    S = Cap->getCapturedStmt();
  return S;
}

/// 取得某个 Expr 在 Rewriter 当前缓冲区中的文本
inline std::string exprText(const clang::Expr *E,
                            const clang::Rewriter &R) {
  return R.getRewrittenText(
      clang::CharSourceRange::getTokenRange(E->getSourceRange()));
}

/// 兼容 clang15/16 对 map(var[:]) 新旧接口差异
template <typename ClauseT, typename MemPtrT>
inline const clang::Expr *firstExpr(ClauseT *C, MemPtrT Getter) {
  using Ret = decltype((C->*Getter)());
  if constexpr (std::is_pointer_v<Ret>)
    return (C->*Getter)();
  else {
    auto Arr = (C->*Getter)();
    return Arr.empty() ? nullptr : Arr.front();
  }
}

/// 把 OpenMP map 类型映射到 SYCL access mode 字符串
inline llvm::StringRef mapTypeToMode(clang::OpenMPMapClauseKind Kind) {
  using namespace clang;
  switch (Kind) {
  case OMPC_MAP_to:     return "sycl::access::mode::read";
  case OMPC_MAP_from:   return "sycl::access::mode::write";
  case OMPC_MAP_tofrom: return "sycl::access::mode::read_write";
  default:              return "sycl::access::mode::read_write";
  }
}

} // namespace Omp2Sycl::detail
