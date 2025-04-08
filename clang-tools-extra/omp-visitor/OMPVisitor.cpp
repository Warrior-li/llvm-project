#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"

using namespace clang;
using namespace clang::tooling;

class MyASTVisitor : public RecursiveASTVisitor<MyASTVisitor> {
public:
    explicit MyASTVisitor(Rewriter &R) : TheRewriter(R) {}

    bool VisitCallExpr(CallExpr *Call) {
        // 检查是否调用了 printf
        if (FunctionDecl *Func = Call->getDirectCallee()) {
            if (Func->getNameInfo().getName().getAsString() == "printf") {
                // 获取 printf 的第一个参数
                if (Call->getNumArgs() > 0) {

                    Expr *Arg = Call->getArg(0);

                    Arg->dump();

                    //jump all implicicast
                    while(ImplicitCastExpr *Cast = dyn_cast<ImplicitCastExpr>(Arg)){
                        Arg = Cast->getSubExpr();
                    }


                    if (StringLiteral *Str = dyn_cast<StringLiteral>(Arg)) {
                        // 构造新的替换代码
                        std::string NewCode = "std::cout << \"" + Str->getString().str() + "\"";

                        llvm::outs() << NewCode << "\n";

                        // 替换原始 printf 语句
                        TheRewriter.ReplaceText(Call->getSourceRange(), NewCode);
                    }
                }
            }
        }
        return true;
    }

private:
    Rewriter &TheRewriter;
};

// 自定义 ASTConsumer
class MyASTConsumer : public ASTConsumer {
public:
    explicit MyASTConsumer(Rewriter &R) : Visitor(R) {}

    void HandleTranslationUnit(ASTContext &Context) override {
        Visitor.TraverseDecl(Context.getTranslationUnitDecl());
    }

private:
    MyASTVisitor Visitor;
};


// 自定义 FrontendAction
class MyFrontendAction : public ASTFrontendAction {
public:
    MyFrontendAction() {}

    void EndSourceFileAction() override {
        TheRewriter.getEditBuffer(TheRewriter.getSourceMgr().getMainFileID()).write(llvm::outs());
    }

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef) override {
        TheRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        return std::make_unique<MyASTConsumer>(TheRewriter);
    }

private:
    Rewriter TheRewriter;
};

// 解析命令行参数
static llvm::cl::OptionCategory ToolCategory("Transformer Options");

int main(int argc, const char **argv) {
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, ToolCategory);
    if (!ExpectedParser) {
    // Fail gracefully for unsupported options.
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser& OptionsParser = ExpectedParser.get();
  ClangTool Tool(OptionsParser.getCompilations(), OptionsParser.getSourcePathList());
  return Tool.run(newFrontendActionFactory<MyFrontendAction>().get());
}
