#pragma once
#include "Omp2Sycl/Rewriters.hpp"
#include <clang/Lex/PPCallbacks.h>

namespace Omp2Sycl {

/// 给主文件自动插入 #include <sycl/sycl.hpp>
class HeaderRewriter final : public clang::PPCallbacks {
public:
  HeaderRewriter(clang::Rewriter &R, clang::SourceManager &SM);

  // --- PPCallbacks overrides ---
  void InclusionDirective(clang::SourceLocation HashLoc,
                          const clang::Token &IncludeTok,
                          llvm::StringRef FileName,
                          bool IsAngled,
                          clang::CharSourceRange FilenameRange,
                          clang::OptionalFileEntryRef File,
                          llvm::StringRef SearchPath,
                          llvm::StringRef RelativePath,
                          const clang::Module *Imported,
                          bool FileIsImport,
                          clang::SrcMgr::CharacteristicKind FileType) override;

  void EndOfMainFile() override;

private:
  clang::Rewriter &TheRewriter;
  clang::SourceManager &SM;
  unsigned LastIncludeLine = 0;
};

} // namespace Omp2Sycl
