#include "OMPDetect.h"
#include "clang/AST/StmtOpenMP.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "clang/AST/ExprOpenMP.h"
#include "clang/Basic/OpenMPKinds.h"


using namespace clang;

// ============ 帮助函数：展开 CapturedStmt ============
static const Stmt* unwrapCaptured(const Stmt* S) {
  while (const auto *Cap = llvm::dyn_cast<CapturedStmt>(S))
    S = Cap->getCapturedStmt();
  return S;
}


/// 返回某个 Expr 在“当前 RewriteBuffer”里的文本
static std::string exprText(const Expr *E, const Rewriter &R) {
  return R.getRewrittenText(
         CharSourceRange::getTokenRange(E->getSourceRange()));
}

template <typename ClauseT, typename MemPtrT>
static const Expr *firstExpr(ClauseT *C, MemPtrT Getter) {
  using Ret = decltype((C->*Getter)());            // 推导返回类型

  if constexpr (std::is_pointer_v<Ret>)            // 旧接口：Expr*
    return (C->*Getter)();
  else {                                           // 新接口：ArrayRef<Expr*>
    auto Arr = (C->*Getter)();
    return Arr.empty() ? nullptr : Arr.front();
  }
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
  const auto *TU = Result.Nodes.getNodeAs<TranslationUnitDecl>("tu");
  if (!TU) return;

  ASTContext &Ctx = *Result.Context;

  for (const Decl *D : TU->decls()) {
    if (const auto *FD = llvm::dyn_cast<FunctionDecl>(D)) {
      if (FD->hasBody())
        RecursiveRewrite(FD->getBody(), Ctx);      // 普通函数
    } else if (const auto *FT = llvm::dyn_cast<FunctionTemplateDecl>(D)) {
      const FunctionDecl *Templated = FT->getTemplatedDecl();
      if (Templated && Templated->hasBody())
        RecursiveRewrite(Templated->getBody(), Ctx);  // 模板定义
    }
  }
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
    // 不替换 omp.h，只记录最后一个 #include 的位置
    if (SM.isInMainFile(HashLoc)) {
      unsigned Line = SM.getSpellingLineNumber(HashLoc);
      LastIncludeLoc = std::max(LastIncludeLoc, Line);
    }
}

void HeaderRewriter::EndOfMainFile() {
  FileID FID = SM.getMainFileID();
  SourceLocation InsertLoc;
  if (LastIncludeLoc > 0) {
    InsertLoc = SM.translateLineCol(FID, LastIncludeLoc + 1, 1);
  } else {
    InsertLoc = SM.getLocForStartOfFile(FID);
  }

  std::string HeaderCode = "#include <sycl/sycl.hpp>\nusing namespace sycl;\n";
  TheRewriter.InsertText(InsertLoc, HeaderCode);
}

void OpenMPRewriter::RecursiveRewrite(const Stmt *Node, ASTContext &Context) {
  if (!Node) return;

  const SourceManager &SM = Context.getSourceManager();
  // ① 先递归普通 children
  for (const Stmt *Child : Node->children())
  RecursiveRewrite(Child, Context);

  // ② 若是 CapturedStmt，再单独递归一次内部的 Captured 部分
  if (const auto *CS = llvm::dyn_cast<CapturedStmt>(Node))
  RecursiveRewrite(CS->getCapturedStmt(), Context);

  // 再处理自己（后序遍历
  if(const auto *EEE = dyn_cast<OMPExecutableDirective>(Node)){
    llvm::outs() << EEE->getStmtClassName() << "\n";
  }

  if (const auto *ParallelFor = dyn_cast<OMPTargetParallelForDirective>(Node)) {
    // 处理 parallel for
    OpenMPRewriter::rewriteParallelFor(ParallelFor, Context);
    return;
  } 
  else 
  if (const auto *TargetData = dyn_cast<OMPTargetDataDirective>(Node)) {
    // 处理 target data
    OpenMPRewriter::rewriteTargetData(TargetData, Context);
    return;
  }
  // else
  // if (const auto *TDPF = dyn_cast<OMPTargetTeamsDistributeParallelForDirective>(Node)){
  //   rewriteTeamsDistributeParallelFor(TDPF, Context);
  //   return;
  // }
  // 如果是 OpenMP 指令但没有显式处理，则删除整个 pragma 块
  else if (const auto *Dir = dyn_cast<OMPTargetUpdateDirective>(Node)) {
    SourceLocation StartLoc = Dir->getBeginLoc();
    SourceLocation EndLoc = Dir->getEndLoc();
  
    CharSourceRange R = CharSourceRange::getTokenRange(StartLoc, EndLoc);
    std::string Str;
    llvm::raw_string_ostream OS(Str);
    OS << " // [OpenMPRewriter] Removed unhandled directive: " << Dir->getStmtClassName() << "\n";
    OS.flush();
  
    // 替换为注释，防止破坏语义结构，也便于定位
    TheRewriter.ReplaceText(R, Str);
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

      llvm::outs() << "[OpenMPRewriter] Handling target parallel for\n";

      const Stmt *Init = ForLoop->getInit();
      const Expr *Cond = ForLoop->getCond();

      std::string IteratorName = "idx"; // 缺省名
      std::string UpperBound = "N";   // 缺省上限

      // 尝试解析 Init 部分
      if (const auto *DS = dyn_cast<DeclStmt>(Init)) {
        if (const auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl())) {
          IteratorName = VD->getNameAsString();
        }
      }

      // ==== 提取上界表达式 ====
      if (const auto *BinOp = dyn_cast_or_null<BinaryOperator>(Cond)) {
        if (BinOp->isRelationalOp()) {
          const Expr *BoundExpr = BinOp->getRHS()->IgnoreParenImpCasts();
          CharSourceRange Range = CharSourceRange::getTokenRange(BoundExpr->getSourceRange());
          UpperBound = Lexer::getSourceText(Range, SM, Context.getLangOpts()).str();
        }
      }

      // ==== 提取循环体代码 ====
      CharSourceRange BodyRange = CharSourceRange::getTokenRange(ForLoop->getBody()->getSourceRange());
      std::string BodyCode = TheRewriter.getRewrittenText(BodyRange);

      // ==== 生成替换代码 ====
      std::string NewCode;
      llvm::raw_string_ostream OS(NewCode);
      OS << "h.parallel_for(sycl::range<1>(" << UpperBound << "), "
        << "[=](sycl::id<1> " << IteratorName << ") {\n"
        << BodyCode << "\n"
        << "});";
      OS.flush();

      // ==== 替换整个 pragma + for 区域 ====
      CharSourceRange FullRange = CharSourceRange::getTokenRange(
          ParallelFor->getBeginLoc(), ForLoop->getEndLoc());
      TheRewriter.ReplaceText(FullRange, NewCode);
    }


void OpenMPRewriter::rewriteTargetData(const OMPTargetDataDirective *TD, ASTContext &Context) {
  const SourceManager &SM = Context.getSourceManager();

  // ---------- 1. 取得 pragma 区间 ----------
  SourceLocation StartLoc = TD->getBeginLoc();
  const Stmt *Body = unwrapCaptured(TD->getAssociatedStmt());
  if (!Body) return;

  // ---------- 2. 把 Body 源码取出来 ----------
  CharSourceRange BodyR = CharSourceRange::getTokenRange(Body->getSourceRange());
  std::string BodyCode =                      // 用 std::string 接收
    TheRewriter.getRewrittenText(
        CharSourceRange::getTokenRange(Body->getSourceRange()));


  std::set<std::string> Seen;
  std::string Prologue;
  llvm::raw_string_ostream PO(Prologue);

  PO << "sycl::queue q;\n";

  for(const OMPClause *C : TD->clauses()){
    if(const auto *MapC = llvm::dyn_cast<OMPMapClause>(C)){
      const char *ModeStr = mapTypeToMode(MapC->getMapType());
      for(const Expr *V : MapC->varlist()){
        const Expr *E  = V->IgnoreParenImpCasts();
        
        // low-b length
        std::string VarName, LB = "0", LEN = "N";
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
  TheRewriter.ReplaceText(
    CharSourceRange::getTokenRange(TD->getBeginLoc(), Body->getEndLoc()),
    Replacement);
}

void OpenMPRewriter::rewriteTeamsDistributeParallelFor(const OMPTargetTeamsDistributeParallelForDirective *TDPF, ASTContext &Context){
  const Rewriter &R = TheRewriter;
  /* ---------- ① 解包 ForStmt ---------- */
  const Stmt *Body = unwrapCaptured(TDPF->getAssociatedStmt());
  if (!Body) return;
  const auto *For = dyn_cast<ForStmt>(Body);
  if (!For) return;

  // k重循环展开为1-D
  unsigned Collapse = 1;                     // default
  if (auto *Col = TDPF->getSingleClause<OMPCollapseClause>())
    if (auto *Lit = dyn_cast<IntegerLiteral>(Col->getNumForLoops()))
      Collapse = Lit->getValue().getZExtValue();

    /* ── ② 解析迭代变量 / 上界(展开后)─────────────────────────── */
  // 为简单起见：仅示范 collapse==1 情况的提取
  std::string It = "i", UB = "N";
  if (auto *DS = dyn_cast<DeclStmt>(For->getInit()))
    if (auto *VD = dyn_cast<VarDecl>(DS->getSingleDecl()))
      It = VD->getNameAsString();
  if (auto *Cond = dyn_cast<BinaryOperator>(For->getCond()))
    if (auto *RHS = dyn_cast<Expr>(Cond->getRHS()))
      UB = exprText(RHS, R);



  std::optional<std::string> NumThreads;
  std::optional<std::string> ThreadLimit;
  std::optional<std::string> NumTeams;

  for (const OMPClause *C : TDPF->clauses()) {
    switch (C->getClauseKind()) {
      case llvm::omp::Clause::OMPC_num_threads:
        ThreadLimit = exprText(dyn_cast<OMPThreadLimitClause>(C)->getThreadLimit().front(), R);
        break;
  
      // case llvm::omp::Clause::OMPC_thread_limit:
      //   safePush(ThreadLimit,
      //            firstExpr(cast<OMPThreadLimitClause>(C),
      //                      &OMPThreadLimitClause::getThreadLimit), R);
      //   break;
  
      // case llvm::omp::Clause::OMPC_num_teams:
      //   safePush(NumTeams,
      //            firstExpr(cast<OMPNumTeamsClause>(C),
      //                      &OMPNumTeamsClause::getNumTeams), R);
        break;
  
      default: break;
    }
  }

  llvm::outs() << ThreadLimit << "\n";

  /* ── ④ 生成 local & global 表达式字符串 ───────────────── */
  // local_range = num_threads  or thread_limit  or device_max
  const std::string FallbackLocal = "deviceMaxWG";
  std::string Local  = NumThreads.value_or(ThreadLimit.value_or(FallbackLocal));

  // 若 Local 本身就是变量名/表达式则直接用；否则需要把 fallback
  // 转成 "std::min(deviceMaxWG, <expr>)" 保证不超过硬件上限
  if (Local != FallbackLocal)
    Local = "std::min<size_t>(" + Local + ", deviceMaxWG)";

  // 若用户没写 num_teams，则用自动 ceil(N/local)
  std::string Teams = NumTeams.value_or(
      "((" + UB + " + " + Local + " - 1) / " + Local + ")");

  std::string Global = "(" + Teams + " * " + Local + ")";

  /* ── ⑤ 读取循环体(最新缓冲区) ───────────────────────────── */
  std::string BodyCode =
      R.getRewrittenText(
        CharSourceRange::getTokenRange(For->getBody()->getSourceRange()));

  /* ── ⑥ 生成 SYCL 代码 ──────────────────────────────────── */
  std::string Out;
  llvm::raw_string_ostream OS(Out);

  // 设备最大 work-group size 查询（只插一次：可对 HeaderRewriter 做）
  OS << "size_t deviceMaxWG = q.get_device().get_info<sycl::info::device::max_work_group_size>();\n";

  OS << "size_t local  = " << Local << ";\n"
     << "size_t global = " << Global << ";\n\n"
     << "q.submit([&](sycl::handler &h){\n"
     << "  h.parallel_for(sycl::nd_range<1>{ sycl::range<1>(global), "
        "sycl::range<1>(local) },\n"
     << "    [=](sycl::nd_item<1> it){\n"
     << "      int " << It << " = it.get_global_id(0);\n"
     << "      if (" << It << " < " << UB << ") {\n"
     << BodyCode << "\n"
     << "      }\n"
     << "    });\n"
     << "}).wait();";

  OS.flush();

  /* ── ⑦ 替换 ────────────────────────────────────────────── */
  TheRewriter.ReplaceText(
      CharSourceRange::getTokenRange(TDPF->getBeginLoc(),
                                     For->getEndLoc()),
      Out);

}
