#include "OMPDetect.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Format/Format.h"

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;
using namespace clang::format;

static llvm::cl::OptionCategory ToolCategory("omp-to-sycl options");

class MyFrontendAction : public ASTFrontendAction {
public:
  void EndSourceFileAction() override {
    const SourceManager &SM = TheRewriter.getSourceMgr();
    FileID                FID = SM.getMainFileID();

    // 1. 取改动后的文本；若文件从未被改动，则取原源码
    std::string Code;
    if (const llvm::RewriteBuffer *RB = TheRewriter.getRewriteBufferFor(FID)) {
      Code.assign(RB->begin(), RB->end());                  // Rewriter 结果
    } else {
      Code = SM.getBufferOrFake(FID).getBuffer().str();                        // 原文件文本
    }

    FormatStyle Style = getLLVMStyle();          // ← 直接 LLVM 风格


    clang::tooling::Range Whole(0, Code.size());
    auto Repls = clang::format::reformat(Style, Code, {Whole});

    // 4. 应用补丁得到排版后代码
    auto Formatted = clang::tooling::applyAllReplacements(Code, Repls);
    if (!Formatted) {
      llvm::errs() << llvm::toString(Formatted.takeError());
      return;
    }

    //------------------------------------------------------------
    // 5. 输出
    //------------------------------------------------------------
    llvm::outs() << *Formatted;
  }


  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef) override {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());

    SM = &CI.getSourceManager();

    // 插入头文件处理器
    CI.getPreprocessor().addPPCallbacks(std::make_unique<HeaderRewriter>(TheRewriter, CI.getSourceManager()));

    auto Callback = std::make_unique<OpenMPRewriter>(TheRewriter);
    Finder.addMatcher(translationUnitDecl().bind("tu"), Callback.get());
    Callbacks.push_back(std::move(Callback));

    return Finder.newASTConsumer();
  }

private:
  Rewriter TheRewriter;
  MatchFinder Finder;
  std::vector<std::unique_ptr<MatchFinder::MatchCallback>> Callbacks;
  clang::SourceManager *SM = nullptr;
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