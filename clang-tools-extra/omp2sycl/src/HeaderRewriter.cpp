#include "Omp2Sycl/HeaderRewriter.hpp"
#include <clang/Rewrite/Core/Rewriter.h>

using namespace clang;

namespace Omp2Sycl {

HeaderRewriter::HeaderRewriter(Rewriter &R, SourceManager &M)
    : TheRewriter(R), SM(M) {}

void HeaderRewriter::InclusionDirective(SourceLocation HashLoc,
                                        const Token &IncludeTok,
                                        llvm::StringRef FileName,
                                        bool /*IsAngled*/,
                                        CharSourceRange /*FilenameRange*/,
                                        OptionalFileEntryRef /*File*/,
                                        llvm::StringRef /*SearchPath*/,
                                        llvm::StringRef /*RelativePath*/,
                                        const Module * /*Imported*/,
                                        bool /*FileIsImport*/,
                                        SrcMgr::CharacteristicKind /*FileTy*/) {
  if (SM.isInMainFile(HashLoc))
    LastIncludeLine =
        std::max(LastIncludeLine, SM.getSpellingLineNumber(HashLoc));
}

void HeaderRewriter::EndOfMainFile() {
  FileID FID = SM.getMainFileID();
  SourceLocation Insert =
      LastIncludeLine ? SM.translateLineCol(FID, LastIncludeLine + 1, 1)
                      : SM.getLocForStartOfFile(FID);

  constexpr llvm::StringLiteral Code =
      "#include <sycl/sycl.hpp>\nusing namespace sycl;\n";
  TheRewriter.InsertText(Insert, Code);
}

std::unique_ptr<IRewriter> makeHeaderRewriter(Rewriter &R,
                                              SourceManager &SM) {
  // 用 unique_ptr<IRewriter> 返回空指针即可，因为 PPCallbacks 不是 IRewriter。
  // 由调用者单独注册到 Preprocessor 上。
  return nullptr;
}

} // namespace Omp2Sycl
