#include "OMPDetect.h"
#include "clang/AST/StmtOpenMP.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/AST/ExprOpenMP.h"

using namespace clang;

// ============ 帮助函数：展开 CapturedStmt ============
static const Stmt* unwrapCaptured(const Stmt* S) {
  while (const auto *Cap = llvm::dyn_cast<CapturedStmt>(S))
    S = Cap->getCapturedStmt();
  return S;
}

// ============ 帮助函数：根据 map 类型返回 access::mode ============
static const char* mapTypeToMode(OpenMPMapClauseKind Kind) {
  switch (Kind) {
    case OMPC_MAP_to:      return "sycl::access::mode::read";
    case OMPC_MAP_from:    return "sycl::access::mode::write";
    case OMPC_MAP_tofrom:  return "sycl::access::mode::read_write";
    default:               return "sycl::access::mode::read_write"; // alloc, delete…
  }
}

OpenMPRewriter::OpenMPRewriter(Rewriter &R) : TheRewriter(R) {}

void OpenMPRewriter::run(const MatchFinder::MatchResult &Result) {
  const auto *S = Result.Nodes.getNodeAs<Stmt>("stmt");
  if (!S) return;

  ASTContext *Context = Result.Context;
  const SourceManager &SM = Context->getSourceManager();
  if (!SM.isWrittenInMainFile(S->getBeginLoc())) return;

  // 这里不要直接修改 AST
  // 开始从这个节点递归向下处理
  RecursiveRewrite(S, *Context);
}

HeaderRewriter::HeaderRewriter(Rewriter &R, SourceManager &SM)
    : TheRewriter(R), SM(SM), LastIncludeLoc(0) {}

    void HeaderRewriter::InclusionDirective(SourceLocation HashLoc,
                                            const Token &IncludeTok,
                                            StringRef FileName,
                                            bool IsAngled,
                                            CharSourceRange FilenameRange,
                                            OptionalFileEntryRef File,
                                            StringRef SearchPath,
                                            StringRef RelativePath,
                                            const Module *Imported,
                                            bool FileIsImport,
                                            SrcMgr::CharacteristicKind FileType) {
      if (FileName == "omp.h") {
        TheRewriter.ReplaceText(FilenameRange.getAsRange(), "<sycl/sycl.hpp>");
      }

      if (SM.isInMainFile(HashLoc)) {
        unsigned Line = SM.getSpellingLineNumber(HashLoc);
        LastIncludeLoc = std::max(LastIncludeLoc, Line);
      }
}

void HeaderRewriter::EndOfMainFile() {
  FileID FID = SM.getMainFileID();
  SourceLocation InsertLoc, QueueLoc;
  if (LastIncludeLoc > 0) {
    // using name space location
    InsertLoc = SM.translateLineCol(FID, LastIncludeLoc + 1, 1);
    // queue location
    QueueLoc = SM.translateLineCol(FID, LastIncludeLoc + 2, 1);
  } else {
    InsertLoc = SM.getLocForStartOfFile(FID);
    QueueLoc = SM.translateLineCol(FID, 1, 1);
  }

  TheRewriter.InsertText(InsertLoc, "using namespace sycl;\n");
}

void OpenMPRewriter::RecursiveRewrite(const Stmt *Node, ASTContext &Context) {
  if (!Node) return;

  const SourceManager &SM = Context.getSourceManager();

  // 先递归处理子节点
  for (const Stmt *Child : Node->children()) {
    RecursiveRewrite(Child, Context);
  }

  // 再处理自己（后序遍历）

  if (const auto *ParallelFor = dyn_cast<OMPTargetParallelForDirective>(Node)) {
    // 处理 parallel for
    OpenMPRewriter::rewriteParallelFor(ParallelFor, Context);
  }
  else if (const auto *Target = dyn_cast<OMPTargetDirective>(Node)) {
    // 处理 target
    SourceLocation Start = Target->getBeginLoc();
    SourceLocation End = Target->getEndLoc();
    CharSourceRange Range = CharSourceRange::getTokenRange(Start, End);
    TheRewriter.ReplaceText(Range, "// converted target\n");
  }
  else if (const auto *TargetData = dyn_cast<OMPTargetDataDirective>(Node)) {
    // 处理 target data
    OpenMPRewriter::rewriteTargetData(TargetData, Context);
  }
}

void OpenMPRewriter::rewriteParallelFor(
    const OMPTargetParallelForDirective *ParallelFor, ASTContext &Context) {
      const SourceManager &SM = Context.getSourceManager();

      const Stmt * Body = ParallelFor->getAssociatedStmt();
      Body = unwrapCaptured(Body);
      if(!Body) return;

      const auto *ForLoop = dyn_cast<ForStmt>(Body);
      if(!ForLoop) return;

      const Stmt *Init = ForLoop->getInit();
      const Expr *Cond = ForLoop->getCond();
      const Expr *Inc = ForLoop->getInc();

      std::string IteratorName = "i"; // 缺省名
      std::string UpperBound = "N";   // 缺省上限

      // 尝试解析 Init 部分
      if (const auto *DS = dyn_cast<DeclStmt>(Init)) {
        if (const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl())) {
          IteratorName = VD->getNameAsString();
        }
      }

      // 尝试解析 Condition 部分
      if (const auto *BinOp = dyn_cast<BinaryOperator>(Cond)) {
        if (BinOp->getOpcode() == BO_LT || BinOp->getOpcode() == BO_LE) { // i < N 或 i <= N
          if (const auto *BoundExpr = dyn_cast<DeclRefExpr>(BinOp->getRHS()->IgnoreParenImpCasts())) {
            UpperBound = BoundExpr->getNameInfo().getAsString();
          }
        }
      }

      SourceLocation ForStart = ForLoop->getBeginLoc();
      SourceLocation ForEnd = ForLoop->getEndLoc();
      CharSourceRange ForRange = CharSourceRange::getTokenRange(ForStart, ForEnd);

      // 读取 for 循环的源代码（保留循环体）
      llvm::StringRef ForCode = Lexer::getSourceText(ForRange, SM, Context.getLangOpts());
      
      const Stmt *LoopBody = ForLoop->getBody();
      if(!LoopBody) return;

      SourceLocation BodyStart = LoopBody->getBeginLoc();
      SourceLocation BodyEnd = LoopBody->getEndLoc();
      CharSourceRange BodyRange = CharSourceRange::getTokenRange(BodyStart, BodyEnd);
      llvm::StringRef LoopBodyCode = Lexer::getSourceText(BodyRange, SM, Context.getLangOpts());


      // 生成 parallel_for 替换代码
      std::string Replacement;
      llvm::raw_string_ostream OS(Replacement);

      OS << "h.parallel_for(sycl::range<1>(" << UpperBound << "), [=](sycl::id<1> " << IteratorName << ") {\n";
      OS << LoopBodyCode << "\n";
      OS << "});";
    
      OS.flush();
    
      // 4. 替换整个 #pragma + for 区域
      SourceLocation Start = ParallelFor->getBeginLoc();
      SourceLocation End = ForLoop->getEndLoc();
      SourceRange ReplaceRange(Start, End);
    
      TheRewriter.ReplaceText(ReplaceRange, Replacement);
    }


void OpenMPRewriter::rewriteTargetData(const OMPTargetDataDirective *TD, ASTContext &Context) {
  const SourceManager &SM = Context.getSourceManager();

  // ---------- 1. 取得 pragma 区间 ----------
  SourceLocation StartLoc = TD->getBeginLoc();
  const Stmt *Body = unwrapCaptured(TD->getAssociatedStmt());
  if (!Body) return;
  SourceLocation EndLoc   = Body->getEndLoc();

  // ---------- 2. 把 Body 源码取出来 ----------
  CharSourceRange BodyR = CharSourceRange::getTokenRange(Body->getSourceRange());
  llvm::StringRef BodyCode = Lexer::getSourceText(BodyR, SM, Context.getLangOpts());

  std::set<std::string> Seen;
  std::string Prologue;
  llvm::raw_string_ostream PO(Prologue);

  PO << "sycl::queue q;\n";

  for(const OMPClause *C : TD->clauses()){
    if(const auto *MapC = llvm::dyn_cast<OMPMapClause>(C)){
      const char *ModeStr = mapTypeToMode(MapC->getMapType());
      for(const Expr *V : MapC->varlist()){
        const Expr *E  = V->IgnoreParenImpCasts();
        
        std::string VarName;
        const Expr *BaseExpr = nullptr;
        const Expr *ExtentExpr = nullptr;

        

        if(const auto *Sec = dyn_cast<ArraySectionExpr>(E)){
          // A[0:N] B[:M]
          BaseExpr = Sec->getBase()->IgnoreParenImpCasts();
          ExtentExpr = Sec->getLength();
        } else if(const auto *Sub = dyn_cast<ArraySubscriptExpr>(E)){
          BaseExpr = Sub->getBase()->IgnoreImpCasts();
        } else {
          BaseExpr = E;
        }

        if(const auto *DR = dyn_cast<DeclRefExpr>(BaseExpr)){
          VarName = DR->getNameInfo().getAsString();
        }

        if (VarName.empty()) continue;   // 无法解析，跳过

        if (!Seen.insert(VarName).second) continue; // 已处理过

        std::string LenStr = "1";

        if(ExtentExpr){
          LenStr = Lexer::getSourceText(CharSourceRange::getTokenRange(ExtentExpr->getSourceRange()),SM,Context.getLangOpts()).str();
        } else {
          LenStr = "N";
        }
        PO << "sycl::buffer " << VarName << "_buf("
        << VarName << ", sycl::range<1>(" << LenStr << "));\n";
      }
    }
  }

  PO.flush();

  // ---------- 4. 生成 queue.submit ----------
  std::string Replacement;
  llvm::raw_string_ostream OS(Replacement);

  OS << Prologue;
  OS << "q.submit([&](sycl::handler &h){\n";

  // 如果你想立刻生成 accessor，可在此处遍历 Seen 集合
  // 示例（只演示 read mode）:
  for (const std::string &Name : Seen)
    OS << "  auto " << Name << " = " << Name
       << "_buf.get_access<sycl::access::mode::read>(h);\n";

  OS << BodyCode << "\n";
  OS << "}).wait();";

  OS.flush();

  // ---------- 5. 一刀替换 ----------
  TheRewriter.ReplaceText(SourceRange(StartLoc, EndLoc), Replacement);

}