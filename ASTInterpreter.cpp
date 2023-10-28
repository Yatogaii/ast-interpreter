//==--- tools/clang-check/ClangInterpreter.cpp - Clang Interpreter tool --------------===//
//===----------------------------------------------------------------------===//

#include <iostream>
#include <sstream>
#include <fstream>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

using namespace clang;

#include "Environment.h"

class InterpreterVisitor : 
   public EvaluatedExprVisitor<InterpreterVisitor> {
public:
   explicit InterpreterVisitor(const ASTContext &context, Environment * env)
   : EvaluatedExprVisitor(context), mEnv(env) {}
   virtual ~InterpreterVisitor() {}

   virtual void VisitBinaryOperator (BinaryOperator * bop) {
	   VisitStmt(bop);
	   mEnv->binop(bop);
   }

    // 字面量整型没有子语句，所以不用 VisitStmt()
    virtual void VisitIntegerLiteral(IntegerLiteral *integer){
//        integer->dump();
        mEnv->integer(integer);
    }

    /// test05.c 维护while语句
    virtual void VisitWhileStmt(WhileStmt * whilestmt) {
        // clang/AST/Stmt.h: class WhileStmt

        Expr * cond = whilestmt->getCond();
        Stmt * body = whilestmt->getBody();

        Visit(cond);

        // 每次循环都要重新 evaluate 一下 condition 的值，以更新 StackFrame 中保存的结果
        while (mEnv->getExprValue(cond)) {
            Visit(body);
            Visit(cond);
        }
    }

    /// tet04.c 报错，看了看 AST 应该是 a=-10前面少了一个单元运算符
    virtual void VisitUnaryOperator(UnaryOperator * uop){
        VisitStmt(uop);
        mEnv->unaop(uop);

   }

    virtual void VisitDeclRefExpr(DeclRefExpr * expr) {
	   VisitStmt(expr);
	   mEnv->declref(expr);
   }
   virtual void VisitCastExpr(CastExpr * expr) {
	   VisitStmt(expr);
	   mEnv->cast(expr);
   }

   /// test03.c 执行完if之后还会执行else，需要做额外操作不让他执行else
   // 添加完这么多之后可以正常获结果
    virtual void VisitIfStmt(IfStmt * ifstmt) {
        Expr * cond = ifstmt->getCond();

        /// 来自：https://github.com/ChinaNuke/ast-interpreter/commit/fe0f6c357ebd105755def932f538b006a919b35b
        // 此处不能用 VisitStmt() 因为它只会取出参数的所有子节点进行遍历而忽略当前节点本身
        // 比如对于一个 BinaryOperator，使用 VisitStmt() 会跳过 VisitBinaryOperator() 的执行
        // 详见：clang/AST/EvaluatedExprVisitor.h: void VisitStmt(PTR(Stmt) S)
        Visit(cond);

        // 根据 cond 判断的结果只去 Visit 需要执行的子树
        if (mEnv->getExprValue(cond)) {
            VisitStmt(ifstmt->getThen());
        } else {
            // 应该可以自动应对没有 Else 分支的情况，未做测试
            VisitStmt(ifstmt->getElse());
        }
    }

   virtual void VisitCallExpr(CallExpr * call) {

	   VisitStmt(call);
	   mEnv->call(call);
   }
   virtual void VisitDeclStmt(DeclStmt * declstmt) {
	   mEnv->decl(declstmt);
   }
private:
   Environment * mEnv;
};

class InterpreterConsumer : public ASTConsumer {
public:
   explicit InterpreterConsumer(const ASTContext& context) : mEnv(),
   	   mVisitor(context, &mEnv) {
   }
   virtual ~InterpreterConsumer() {}

   virtual void HandleTranslationUnit(clang::ASTContext &Context) {
	   TranslationUnitDecl * decl = Context.getTranslationUnitDecl();
	   mEnv.init(decl);

	   FunctionDecl * entry = mEnv.getEntry();
	   mVisitor.VisitStmt(entry->getBody());
  }
private:
   Environment mEnv;
   InterpreterVisitor mVisitor;
};

class InterpreterClassAction : public ASTFrontendAction {
public: 
  virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
    clang::CompilerInstance &Compiler, llvm::StringRef InFile) {
    return std::unique_ptr<clang::ASTConsumer>(
        new InterpreterConsumer(Compiler.getASTContext()));
  }
};

int main (int argc, char ** argv) {
    if (argc > 1) {
        clang::tooling::runToolOnCode(std::unique_ptr<clang::FrontendAction>(new InterpreterClassAction), argv[1]);
    } else {
        std::string filename("../test/test");
        std::string index;
        std::cout << "请输入测试文件编号：00" << std::endl;
        std::cin >> index;
        filename.append(index).append(".c");
        std::ifstream t(filename);
        std::string buffer((std::istreambuf_iterator<char>(t)),
                           std::istreambuf_iterator<char>());
        std::cout << buffer << std::endl;
        clang::tooling::runToolOnCode(std::unique_ptr<clang::FrontendAction>(new InterpreterClassAction), buffer);
    }
//   if (argc > 1) {
//       clang::tooling::runToolOnCode(std::unique_ptr<clang::FrontendAction>(new InterpreterClassAction), argv[1]);
//   }
}
