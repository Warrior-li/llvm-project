#include "Omp2Sycl/OpenMPRewriter.hpp"
#include "Omp2Sycl/Utils.hpp"

#include <clang/AST/StmtOpenMP.h>
#include <clang/AST/ASTContext.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Lex/Lexer.h>
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

  if (const auto *CS = dyn_cast<CapturedStmt>(Node))
    recursiveRewrite(CS->getCapturedStmt(), Ctx);

  if (const auto *PF = dyn_cast<OMPTargetParallelForDirective>(Node))
    return rewriteParallelFor(PF, Ctx);

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
                                       ASTContext &Context) {
  using namespace detail;
  const SourceManager &SM = Context.getSourceManager();

  // 记录变量的信息
  struct Info {std::string Len, Mode; int Pri;};
  // —— 收集独立变量名 → buffer / accessor 生成
  std::unordered_map<std::string, Info> Vars;

  //合并函数
  auto merge = [&](std::string Name, std::string Len, llvm::StringRef Mode, int Pri){
    auto &I = Vars[Name];
    if(I.Len.empty()) I.Len = std::move(Len);
    if(Pri > I.Pri) {I.Mode = Mode.str(); I.Pri = Pri;}
  };

  const Stmt *BodyStmt = unwrapCaptured(TD->getAssociatedStmt());
  if (!BodyStmt) return;

  std::string BodyCode = TheRewriter.getRewrittenText(
      CharSourceRange::getTokenRange(BodyStmt->getSourceRange()));

  std::string Prologue;
  llvm::raw_string_ostream PO(Prologue);
  PO << "sycl::queue q;\n";


  for (const OMPClause *C : TD->clauses()) {
    auto *MC = dyn_cast<OMPMapClause>(C);
    if(!MC) continue;

    llvm::StringRef Mode = mapTypeToMode(MC->getMapType());
    int Prio = (MC->getMapType() == OMPC_MAP_to) ? 1 : (MC->getMapType() == OMPC_MAP_from ) ? 2 : 3;

    for(const Expr *RawE : MC->varlist()){
      const Expr *E = RawE->IgnoreParenImpCasts();
      const Expr *Base = E;
      const Expr *Len  = nullptr;            // ← 我们要找的“元素个数”表达式

      /* ---- ① A[:len] or A[lb:len] ---- */
      if (auto *Sec = dyn_cast<ArraySectionExpr>(E)) {
        Base = Sec->getBase()->IgnoreParenImpCasts();
        Len = Sec->getLength();                        // A[:Len]
        Len->dump();
      }


    }
  }
  PO.flush();

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << Prologue;
  OS << "q.submit([&](sycl::handler &h){\n";
  for (const auto &V : Vars)
    OS << "  auto " << V << " = " << V
       << "_buf.get_access<sycl::access::mode::read_write>(h);\n";
  OS << BodyCode << "\n}).wait();";
  OS.flush();

  TheRewriter.ReplaceText(
      CharSourceRange::getTokenRange(TD->getBeginLoc(),
                                     BodyStmt->getEndLoc()),
      Out);
}

/* ──────────────────────────────────────────────────────────────────────── */

std::unique_ptr<IRewriter> makeOpenMPRewriter(Rewriter &R) {
  return std::make_unique<OpenMPRewriter>(R);
}

} // namespace Omp2Sycl
