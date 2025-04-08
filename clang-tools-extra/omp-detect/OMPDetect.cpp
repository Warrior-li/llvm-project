// Declares clang::SyntaxOnlyAction.
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
// Declares llvm::cl::extrahelp.
#include "llvm/Support/CommandLine.h"

// ASTMathcers
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchersInternal.h"
#include "clang/AST/StmtOpenMP.h"
#include "clang/AST/ASTContext.h"
// Transformer
#include "clang/Tooling/Transformer/RewriteRule.h"
#include "clang/Tooling/Transformer/RangeSelector.h"
#include "clang/Tooling/Transformer/Stencil.h"
#include "clang/Rewrite/Core/Rewriter.h"


using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

StatementMatcher OpenMPMatcher = stmt().bind("stmt");

class OpenMPPrinter : public MatchFinder::MatchCallback {
public:
  
  virtual void run(const MatchFinder::MatchResult &Result) {
    ASTContext *Context = Result.Context;
    const Stmt *S = Result.Nodes.getNodeAs<Stmt>("stmt");
    
    // 确保节点非空
    if (!S) return;

    // 仅处理主文件中的 OpenMP 语句
    if (!Context->getSourceManager().isWrittenInMainFile(S->getBeginLoc()))
      return;


    // // 检测 `OMPTargetParallelDirective`
    // if (const OMPTargetParallelDirective *EDD = dyn_cast<OMPTargetParallelDirective>(S)) {
    //   showCode(Context->getSourceManager(),EDD);
    //   return;
    // }

    // 检测 `OMPTargetDataDirective`
    if (const OMPTargetDataDirective *EDD = dyn_cast<OMPTargetDataDirective>(S)) {
      showCode(Context->getSourceManager(),EDD);
      analyzeOTDD(EDD);
      return;
    }
    
  }
  
  private:
  std::vector<std::pair<SourceLocation, SourceLocation>> ToDelete;
  void analyzeOTDD(const OMPTargetDataDirective * OTDD){
    llvm::outs() << "\nClause:\n";
    for(const OMPClause * Clause : OTDD->clauses()){
      // llvm::outs() << Clause->getClauseKind();
      for(const Stmt * stmt : Clause->used_children()){
        if(stmt){
          llvm::outs() << getArrayName(dyn_cast<ArraySectionExpr>(stmt)) << " ";
          llvm::outs() << getArrayStartIndex(dyn_cast<ArraySectionExpr>(stmt)) << " ";
          llvm::outs() << getArrayCopySize(dyn_cast<ArraySectionExpr>(stmt));
        }
        llvm::outs() << "\n";
      }
    }
  }

  std::string getArrayCopySize(const ArraySectionExpr * ASE){
    if (const ImplicitCastExpr *CastExpr = dyn_cast<ImplicitCastExpr>(ASE->getLength())) {
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(CastExpr->getSubExpr()))
            return DRE->getNameInfo().getAsString();
    }
    return "";
  }

  int getArrayStartIndex(const ArraySectionExpr *ASE) {
    if (const IntegerLiteral *IntLit = dyn_cast<IntegerLiteral>(ASE->getLowerBound()))
        return IntLit->getValue().getZExtValue();
    return -1;  // 如果没有提供起始索引，默认是 0
  }

  std::string getArrayName(const ArraySectionExpr *ASE) {
    if (const ImplicitCastExpr *CastExpr = dyn_cast<ImplicitCastExpr>(ASE->getBase())) {
        if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(CastExpr->getSubExpr()))
            return DRE->getNameInfo().getAsString();
    }
    return "";
  }

  void showCode(SourceManager &SM, const OMPExecutableDirective *OED){
      SourceLocation StartLoc = OED->getBeginLoc();
      SourceLocation EndLoc = OED->getEndLoc();
      // 获取行号
      unsigned StartLine = SM.getSpellingLineNumber(StartLoc);
      unsigned EndLine = SM.getSpellingLineNumber(EndLoc);


      // 获取源代码
      llvm::StringRef Code = llvm::StringRef(SM.getCharacterData(StartLoc),
                                             SM.getCharacterData(EndLoc) - SM.getCharacterData(StartLoc));

      llvm::outs() << "Found OpenMP Target Teams Directive at Lines: " 
                   << StartLine << "-" << EndLine << "\n";
      llvm::outs() << "Source Code:\n" << Code << "\n\n";

      llvm::outs() << "AST Structure:\n";

      OED->dump();

      ToDelete.push_back({StartLoc,EndLoc});
  }
};







// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static llvm::cl::OptionCategory MyToolCategory("my-tool options");

// CommonOptionsParser declares HelpMessage with a description of the common
// command-line options related to the compilation database and input files.
// It's nice to have this help message in all tools.
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// A help message for this specific tool can be added afterwards.
static cl::extrahelp MoreHelp("\nMore help text...\n");

int main(int argc, const char **argv) {
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
  if (!ExpectedParser) {
    // Fail gracefully for unsupported options.
    llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser& OptionsParser = ExpectedParser.get();
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());

  OpenMPPrinter Printer;

  MatchFinder Finder;
  Finder.addMatcher(OpenMPMatcher, &Printer);

  if (Tool.run(newFrontendActionFactory(&Finder).get()) != 0) {
    return 1;
  }

  return 0;
}