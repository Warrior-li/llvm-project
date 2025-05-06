//===--- omp2sycl.cpp -----------------------------------------------------===//
#include "Omp2Sycl/HeaderRewriter.hpp"   // ① 你的头文件
#include "Omp2Sycl/OpenMPRewriter.hpp"

#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Format/Format.h>

#include <vector>                        // ② 记得引入 <vector>

using namespace clang;
using namespace clang::tooling;
using namespace clang::ast_matchers;

static llvm::cl::OptionCategory ToolCat("omp2sycl options");

//===----------------------------------------------------------------------===//
// 自定义 FrontendAction：把 PPCallbacks + MatchFinder + Rewriter 绑在一起
//===----------------------------------------------------------------------===//
class MyFrontendAction : public ASTFrontendAction {
  Rewriter                    TheRewriter;
  MatchFinder                 Finder;
  std::vector<std::unique_ptr<MatchFinder::MatchCallback>> Callbacks;

public:
  //----- EndSourceFileAction：格式化并输出 ----------------------------------//
  void EndSourceFileAction() override {
    const SourceManager &SM = TheRewriter.getSourceMgr();
    FileID FID = SM.getMainFileID();

    // 1. 拿到重写后的代码（若未改写则用原文）
    std::string Code;
    if (const auto *RB = TheRewriter.getRewriteBufferFor(FID))
      Code.assign(RB->begin(), RB->end());
    else
      Code = SM.getBufferOrFake(FID).getBuffer().str();

    // 2. clang-format （LLVM 风格）
    clang::format::FormatStyle Style = clang::format::getLLVMStyle();
    tooling::Range Whole(0, Code.size());
    auto Repls     = clang::format::reformat(Style, Code, {Whole});
    auto Formatted = tooling::applyAllReplacements(Code, Repls);
    if (!Formatted) {
      llvm::errs() << llvm::toString(Formatted.takeError());
      return;
    }
    // llvm::outs() << *Formatted;
  }

  //----- CreateASTConsumer：注册 HeaderRewriter 与 OpenMPRewriter ----------//
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 llvm::StringRef) override {
    TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());

    // （1）预处理阶段插入头文件转换
    CI.getPreprocessor().addPPCallbacks(
        std::make_unique<Omp2Sycl::HeaderRewriter>(TheRewriter,
                                                   CI.getSourceManager()));

    // （2）AST 匹配阶段的 OpenMP ➜ SYCL 转换
    auto CB = std::make_unique<Omp2Sycl::OpenMPRewriter>(TheRewriter);
    Finder.addMatcher(translationUnitDecl().bind("tu"), CB.get());
    Callbacks.push_back(std::move(CB));

    return Finder.newASTConsumer();
  }
};

//===----------------------------------------------------------------------===//
// main
//===----------------------------------------------------------------------===//
int main(int argc, const char **argv) {
  auto Opts = CommonOptionsParser::create(argc, argv, ToolCat);
  if (!Opts) { llvm::errs() << Opts.takeError(); return 1; }

  ClangTool Tool(Opts->getCompilations(), Opts->getSourcePathList());

  // ③ 只有一个参数版本的 run() —— 把自定义 FrontendAction 交进去
  return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}
