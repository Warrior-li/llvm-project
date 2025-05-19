#include "Omp2Sycl/OpenMPRewriter.hpp"
#include "Omp2Sycl/Utils.hpp"

#include <clang/AST/StmtOpenMP.h>
#include <clang/AST/ASTContext.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/SmallString.h>
#include <unordered_map>

using namespace clang;
using namespace clang::ast_matchers;
using namespace Omp2Sycl::detail;

namespace Omp2Sycl {

OpenMPRewriter::OpenMPRewriter(Rewriter &R) : TheRewriter(R) {}

void OpenMPRewriter::run(const MatchFinder::MatchResult &Result) {
  const auto *TU = Result.Nodes.getNodeAs<TranslationUnitDecl>("tu");
  if (!TU) return;

  ASTContext &Ctx = *Result.Context;
  for (const Decl *D : TU->decls()) {
    if (const auto *FD = dyn_cast<FunctionDecl>(D); FD && FD->hasBody())
      recursiveRewrite(FD->getBody(), Ctx);
    if (const auto *FT = dyn_cast<FunctionTemplateDecl>(D)) {
      const FunctionDecl *Templated = FT->getTemplatedDecl();
      if (Templated && Templated->hasBody())
        recursiveRewrite(Templated->getBody(), Ctx);
    }
  }
}

void OpenMPRewriter::recursiveRewrite(const Stmt *Node, ASTContext &Ctx) {
  if (!Node) return;
  for (const Stmt *Child : Node->children())
    recursiveRewrite(Child, Ctx);

  // if (const auto *CS = dyn_cast<CapturedStmt>(Node))
  //   recursiveRewrite(CS->getCapturedStmt(), Ctx);

  // if (const auto *PF = dyn_cast<OMPTargetParallelForDirective>(Node))
  //   return rewriteParallelFor(PF, Ctx);

  if (const auto *TD = dyn_cast<OMPTargetDataDirective>(Node))
    return rewriteTargetData(TD, Ctx);
}

/* ─────────────────────────────  parallel for  ─────────────────────────── */

void OpenMPRewriter::rewriteParallelFor(
    const OMPTargetParallelForDirective *ParallelFor, ASTContext &Context) {

  const SourceManager &SM = Context.getSourceManager();
  const Stmt *Body = unwrapCaptured(ParallelFor->getAssociatedStmt());
  if (!Body) return;
  const auto *ForLoop = dyn_cast<ForStmt>(Body);
  if (!ForLoop) return;

  /* ① 解析迭代变量 / 上界 */
  std::string It = "idx";
  std::string UB = "N";

  if (const auto *DS = dyn_cast<DeclStmt>(ForLoop->getInit()))
    if (const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl()))
      It = VD->getNameAsString();

  if (const auto *BinOp = dyn_cast_or_null<BinaryOperator>(ForLoop->getCond());
      BinOp && BinOp->isRelationalOp()) {
    const Expr *Bound = BinOp->getRHS()->IgnoreParenImpCasts();
    UB = Lexer::getSourceText(CharSourceRange::getTokenRange(
                                  Bound->getSourceRange()),
                              SM, Context.getLangOpts())
             .str();
  }

  /* ② 循环体代码 */
  std::string BodyCode =
      TheRewriter.getRewrittenText(CharSourceRange::getTokenRange(
          ForLoop->getBody()->getSourceRange()));

  /* ③ 生成新代码 */
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << "h.parallel_for(sycl::range<1>(" << UB << "), "
     << "[=](sycl::id<1> " << It << "){\n"
     << BodyCode << "\n});";
  OS.flush();

  /* ④ 整块替换 */
  TheRewriter.ReplaceText(
      CharSourceRange::getTokenRange(ParallelFor->getBeginLoc(),
                                     ForLoop->getEndLoc()),
      Out);
}

/* ──────────────────────────────  target data  ─────────────────────────── */
void OpenMPRewriter::rewriteTargetData(const OMPTargetDataDirective *TD,
                                       ASTContext &Ctx) {
  using namespace detail;
  const SourceManager &SM = Ctx.getSourceManager();

  // -------- 0. 收集变量信息 (Name→{LenExpr, Mode, Pri}) ---------------- //
  struct Info { std::string Len, Mode; int Pri = 0; };
  std::unordered_map<std::string, Info> Vars;

  auto merge = [&](std::string Name, std::string Len,
                   llvm::StringRef Mode, int Pri) {
    auto &I = Vars[Name];
    if (I.Len.empty()) I.Len = std::move(Len);
    if (Pri > I.Pri) { I.Mode = Mode.str(); I.Pri = Pri; }
  };

  for (const OMPClause *C : TD->clauses()) {
    const auto *MC = dyn_cast<OMPMapClause>(C);
    if (!MC) continue;

    llvm::StringRef Mode = mapTypeToMode(MC->getMapType());
    int Pri = (MC->getMapType() == OMPC_MAP_to   ) ? 1 :
              (MC->getMapType() == OMPC_MAP_from ) ? 2 : 3;


    for (const Expr *Raw : MC->varlist()) {

      // ---- a. 解析表达式 ------------------------------------------------ //
      const Expr *E    = Raw->IgnoreParenImpCasts();
      const Expr *Base = E;
      const Expr *LenE = nullptr;          // 切片长度表达式 (may be null)
      bool IsSubscript = false;

      if (auto *Sec = dyn_cast<ArraySectionExpr>(E)) {
        Base = Sec->getBase()->IgnoreParenImpCasts();
        LenE = Sec->getLength();           // A[:len] A[lb:len]
      }
      else if (auto *Sub = dyn_cast<ArraySubscriptExpr>(E)) {
        Base = Sub->getBase()->IgnoreImpCasts();
        IsSubscript = true;                // A[idx]
      }

      const auto *DR = dyn_cast<DeclRefExpr>(Base);
      if (!DR) continue;                   // 复杂表达式暂不支持

      // 打印源代码
      clang::CharSourceRange Ran = clang::CharSourceRange::getTokenRange(DR->getBeginLoc(),DR->getEndLoc());
      llvm::outs() << Lexer::getSourceText(Ran, SM, Ctx.getLangOpts()) << "\n\n";

      std::string Name = DR->getNameInfo().getAsString();
      std::string LenStr;

      // ---- b. 决定元素个数表达式 --------------------------------------- //
      if (IsSubscript) {
        LenStr = "1";                      // 单元素
      } else if (LenE) {
        LenStr = Lexer::getSourceText(
                   CharSourceRange::getTokenRange(LenE->getSourceRange()),
                   SM, Ctx.getLangOpts()).str();
        if (LenStr.empty())
          LenStr = "/* TODO:len_" + Name + " */";
      } else if (const auto *CAT =
                 dyn_cast<ConstantArrayType>(DR->getType().getTypePtr())) {
        llvm::APInt Sz = CAT->getSize();    // 元素个数（任意位宽无符号整数）
        if (Sz.getActiveBits() <= 63) {
          // ≤64 bit 时可直接转成 uint64，再用 std::to_string
          LenStr = std::to_string(Sz.getZExtValue());
        } else {
          // 大整数：用 SmallString 接收
          llvm::SmallString<32> Tmp;
          Sz.toString(Tmp, /*Radix=*/10, /*Signed=*/false);
          LenStr = std::string(Tmp);
        }
      } else {
        LenStr = "/* TODO:len_" + Name + " */";   // 退化指针
      }

      merge(Name, LenStr, Mode, Pri);
    }
  }

  // -------- 1. 生成重写代码 ------------------------------------------------ //
  std::string Prologue; llvm::raw_string_ostream PO(Prologue);
  PO << "sycl::queue q;\n";
  for (auto &[Name, I] : Vars)
    PO << "sycl::buffer " << Name << "_buf(" << Name
       << ", sycl::range<1>(" << I.Len << "));\n";
  PO.flush();

  // 原始 pragma body
  const Stmt *BodyStmt = unwrapCaptured(TD->getAssociatedStmt());
  if (!BodyStmt) return;
  std::string BodyCode = TheRewriter.getRewrittenText(
      CharSourceRange::getTokenRange(BodyStmt->getSourceRange()));

  std::string Out; llvm::raw_string_ostream OS(Out);
  OS << Prologue;
  OS << "q.submit([&](sycl::handler &h){\n";
  for (auto &[Name, I] : Vars)
    OS << "  auto " << Name << " = " << Name
       << "_buf.get_access<sycl::access::mode::" << I.Mode << ">(h);\n";
  OS << BodyCode << "\n}).wait();";
  OS.flush();

  llvm::outs() << Out;

  TheRewriter.ReplaceText(
      CharSourceRange::getTokenRange(TD->getBeginLoc(), BodyStmt->getEndLoc()),
      Out);
}

}