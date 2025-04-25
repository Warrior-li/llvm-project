#include "OMPDetect.h"
#include "clang/AST/StmtOpenMP.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"

using namespace clang;

OpenMPRewriter::OpenMPRewriter(Rewriter &R) : TheRewriter(R) {}

void OpenMPRewriter::run(const MatchFinder::MatchResult &Result) {
  const auto *S = Result.Nodes.getNodeAs<Stmt>("stmt");
  if (!S) return;

  ASTContext *Context = Result.Context;
  const SourceManager &SM = Context->getSourceManager();
  if (!SM.isWrittenInMainFile(S->getBeginLoc())) return;

  if (const auto *OED = dyn_cast<OMPTargetDataDirective>(S)) {
    SourceRange Range(OED->getBeginLoc(), OED->getEndLoc());

    std::string Replacement = R"cpp(
    queue q;
    sycl::buffer A_buf(A, sycl::range<1>(N));
    q.submit([&](handler &h) {
      // original OpenMP code block
    }); q.wait();
    )cpp";

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