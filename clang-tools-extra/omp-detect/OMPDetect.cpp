#include "OMPDetect.h"
#include "clang/AST/StmtOpenMP.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"

using namespace clang;

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
  TheRewriter.InsertText(InsertLoc, "\nsycl::queue q\n");
}

void OpenMPRewriter::RecursiveRewrite(const Stmt *Node, ASTContext &Context) {
  if (!Node) return;

  const SourceManager &SM = Context.getSourceManager();

  // 先递归处理子节点
  for (const Stmt *Child : Node->children()) {
    RecursiveRewrite(Child, Context);
  }

  // 再处理自己（后序遍历）

  if (const auto *ParallelFor = dyn_cast<OMPParallelForDirective>(Node)) {
    // 处理 parallel for
    SourceLocation Start = ParallelFor->getBeginLoc();
    SourceLocation End = ParallelFor->getEndLoc();
    CharSourceRange Range = CharSourceRange::getTokenRange(Start, End);
    TheRewriter.ReplaceText(Range, "// converted parallel for\n");
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
    SourceLocation Start = TargetData->getBeginLoc();
    SourceLocation End = TargetData->getEndLoc();
    CharSourceRange Range = CharSourceRange::getTokenRange(Start, End);
    TheRewriter.ReplaceText(Range, "// converted target data\n");
  }
}