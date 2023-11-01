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
        //integer->dump();
        mEnv->integer(integer);
    }

    /// test05.c 维护while语句
    virtual void VisitWhileStmt(WhileStmt * whilestmt) {
       //whilestmt->dump();
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

   /// test18.c 支持括号
   /// test19.c 啥都没干19也过了g
   virtual void VisitParenExpr(ParenExpr* pop){
        VisitStmt(pop);
        mEnv->paren(pop);
   }

   /// test17.c 添加 sizeof 的支持
   virtual void VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr* expr){
       VisitStmt(expr);
       mEnv->uette(expr);
   }

    virtual void VisitDeclRefExpr(DeclRefExpr * expr) {
	   VisitStmt(expr);
	   mEnv->declref(expr);
   }
   virtual void VisitCastExpr(CastExpr * expr) {
	   VisitStmt(expr);
	   mEnv->cast(expr);
   }

   /// test12.c 开始着手支持数组
    virtual void VisitArraySubscriptExpr(ArraySubscriptExpr *arrayExpr) {
       // 下面这个 VisitStmt 是为了计算 Base和 Idx
        //VisitStmt(arrayExpr);
        Visit(arrayExpr->getBase());
       Visit(arrayExpr->getIdx());
        mEnv->array(arrayExpr);
    }


    /// test03.c 执行完if之后还会执行else，需要做额外操作不让他执行else
   // 添加完这么多之后可以正常获结果
    virtual void VisitIfStmt(IfStmt * ifstmt) {
        //ifstmt->dump();
        Expr * cond = ifstmt->getCond();

        /// 来自：https://github.com/ChinaNuke/ast-interpreter/commit/fe0f6c357ebd105755def932f538b006a919b35b
        // 此处不能用 VisitStmt() 因为它只会取出参数的所有子节点进行遍历而忽略当前节点本身
        // 比如对于一个 BinaryOperator，使用 VisitStmt() 会跳过 VisitBinaryOperator() 的执行
        // 详见：clang/AST/EvaluatedExprVisitor.h: void VisitStmt(PTR(Stmt) S)
        Visit(cond);

        // 根据 cond 判断的结果只去 Visit 需要执行的子树
        if (mEnv->getExprValue(cond)) {
            //ifstmt->getThen()->dump();
            /// test10.c 为什么这里把VisitStmt换成Visit就能正确获取结果了？
            // VisitStmt(ifstmt->getThen());
            Visit(ifstmt->getThen());
        } else {
            if (Stmt * elseStmt = ifstmt->getElse()) {
                Visit(elseStmt);
            }
        }
    }

    virtual void VisitForStmt(ForStmt * forstmt) {
        // clang/AST/Stmt.h: class ForStmt

        Stmt * init = forstmt->getInit();
        Expr * cond = forstmt->getCond();
        Expr * inc = forstmt->getInc();
        Stmt * body = forstmt->getBody();

        if(init) Visit(init);
        if(cond) Visit(cond);

        // 每次循环都要重新 evaluate 一下 condition 的值，以更新 StackFrame 中保存的结果
        while (mEnv->getExprValue(cond)) {
            Visit(body);
            Visit(inc);
            Visit(cond);
        }
    }


    /// test14.c 还是会报错：int StackFrame::getDeclVal(clang::Decl*): Assertion `mVars.find(decl) != mVars.end()' failed.
   virtual void VisitCallExpr(CallExpr * call) {
       // 为了区分内建函数和正常函数，需要额外判断。
       FunctionDecl * callee = call->getDirectCallee();
       if(callee == mEnv->mFree || callee == mEnv->mOutput || callee == mEnv->mInput || callee == mEnv->mMalloc){
           VisitStmt(call);
           mEnv->call(call);
       } else { // 非自建函数，新建一个函数堆栈之后再 visit
           // 这里必须先visit，要不之后的获取参数会报错
           VisitStmt(call);
           StackFrame newFrame = StackFrame();

           /// test21.c 递归函数时找不到 decl 对应的值
           // 这两个一样吗 一样
           // Decl* test1 = callee->getParamDecl(0);
           // Decl* test2 = llvm::dyn_cast<ParmVarDecl>(callee->getParamDecl(0));
           for (int i = 0; i <  callee->getNumParams(); i++) {
               newFrame.bindDecl(
                       //callee->getParamDecl(i),
                        llvm::dyn_cast<ParmVarDecl>(callee->getDefinition()->getParamDecl(i)),
                       mEnv->mStack.back().getStmtVal(call->getArg(i))
               );
           }
            mEnv->mStack.push_back(newFrame);
            mEnv->depth ++;
           /// test14.c 这里一定要 visit body 否则会报错。
           VisitStmt(call->getDirectCallee()->getBody());
           /// test21.c 这里就不调用 mEnv->call 了
           // mEnv->call(call);
           // 先获取返回值
           int ret = mEnv->mStack.back().getReturnValue();
           mEnv->mStack.pop_back();
           mEnv->depth --;
           mEnv->mStack.back().bindStmt(call, ret);
       }
   }

   // 需要保存返回值
    virtual void VisitReturnStmt(ReturnStmt * ret) {
        // 计算返回值，但不在此处进行返回操作，因为有的函数可能不含有 ReturnStmt.

        VisitStmt(ret);

        // clang/AST/Stmt.h: class ReturnStmt
        mEnv->retstmt(ret);
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
