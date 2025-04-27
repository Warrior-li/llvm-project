#ifndef OPENMP_DETECH_H
#define OPENMP_DETECH_H

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Lex/PPCallbacks.h"


using namespace clang;
using namespace clang::ast_matchers;

class OpenMPRewriter : public MatchFinder::MatchCallback {
    public:
      OpenMPRewriter(Rewriter &R);
      void run(const MatchFinder::MatchResult &Result) override;
      void RecursiveRewrite(const Stmt *Node, ASTContext &Context);
    
    private:
      Rewriter &TheRewriter;
};
    
class HeaderRewriter : public PPCallbacks {
    public:
      HeaderRewriter(Rewriter &R, SourceManager &SM);
      void InclusionDirective(SourceLocation HashLoc,
                              const Token &IncludeTok,
                              StringRef FileName,
                              bool IsAngled,
                              CharSourceRange FilenameRange,
                              OptionalFileEntryRef File,
                              StringRef SearchPath,
                              StringRef RelativePath,
                              const Module *Imported,
                              bool FileIsImport,
                              SrcMgr::CharacteristicKind FileType) override;
      void EndOfMainFile() override;

    private:
      Rewriter &TheRewriter;
      SourceManager &SM;
      unsigned LastIncludeLoc;
};


class OMPDirectiveInfo {
  const Stmt *Node;
  enum class Kind {TargetData, Target, ParalleFor } Type;
};

#endif // OPENMP_H