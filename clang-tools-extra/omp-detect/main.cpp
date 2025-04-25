#include "OMPDetect.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;

static llvm::cl::OptionCategory ToolCategory("omp-to-sycl options");

class MyFrontendAction : public ASTFrontendAction {
public:
  void EndSourceFileAction() override {
    FileID FID = TheRewriter.getSourceMgr().getMainFileID();
    TheRewriter.getEditBuffer(FID).write(llvm::outs());
  }

  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef) override {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());

    // 插入头文件处理器
    CI.getPreprocessor().addPPCallbacks(std::make_unique<HeaderRewriter>(TheRewriter, CI.getSourceManager()));

    auto Callback = std::make_unique<OpenMPRewriter>(TheRewriter);
    Finder.addMatcher(stmt().bind("stmt"), Callback.get());
    Callbacks.push_back(std::move(Callback));

    return Finder.newASTConsumer();
  }

private:
  Rewriter TheRewriter;
  MatchFinder Finder;
  std::vector<std::unique_ptr<MatchFinder::MatchCallback>> Callbacks;
};

int main(int argc, const char **argv) {
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
  if (!ExpectedParser) {
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }

  ClangTool Tool(ExpectedParser->getCompilations(),
                 ExpectedParser->getSourcePathList());
  return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}