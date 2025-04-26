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

  if(const auto *OED = dyn_cast<OMPExecutableDirective>(S)){
    SourceRange Range = OED->getSourceRange();
    Range.getBegin().dump(SM);
    Range.getEnd().dump(SM);
;  }

  if (const auto *OED = dyn_cast<OMPTargetDataDirective>(S)) {
    SourceRange Range = OED->getSourceRange();
    const Stmt *Body = OED->getAssociatedStmt();

    // 如果Body为空，则不修改
    if(!Body) return;

    SourceLocation PragmaLoc = OED->getBeginLoc();
    SourceLocation BodyStart = Body->getBeginLoc();
    SourceLocation BodyEnd = Body->getEndLoc();

    // 提取原始代码为文本ni
    CharSourceRange BodyRange = CharSourceRange::getTokenRange(BodyStart, BodyEnd);
    llvm::StringRef OriginalCode = Lexer::getSourceText(BodyRange, SM, Context->getLangOpts());

    std::string Replacement;
    llvm::raw_string_ostream OS(Replacement);
    OS << "sycl::queue q;\n";
    OS << "q.submit([&](sycl::handler &h) {\n";
    OS << OriginalCode << "\n";
    OS << "}).wait();";

    OS.flush(); // 刷新到 Replacement 字符串中

    TheRewriter.ReplaceText(Range, Replacement);
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
      SourceLocation InsertLoc;
      if (LastIncludeLoc > 0) {
        InsertLoc = SM.translateLineCol(FID, LastIncludeLoc + 1, 1);
      } else {
        InsertLoc = SM.getLocForStartOfFile(FID);
      }
      TheRewriter.InsertText(InsertLoc, "using namespace sycl;\n");
    }