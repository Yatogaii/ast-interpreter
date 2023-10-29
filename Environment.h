//==--- tools/clang-check/ClangInterpreter.cpp - Clang Interpreter tool --------------===//
//===----------------------------------------------------------------------===//
#include <stdio.h>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"

using namespace clang;

class StackFrame {
   /// StackFrame maps Variable Declaration to Value
   /// Which are either integer or addresses (also represented using an Integer value)
   std::map<Decl*, int> mVars;
   std::map<Stmt*, int> mExprs;
   /// The current stmt
   Stmt * mPC;
   int returnValue;
public:
    void setReturnValue(int ret){ returnValue=ret;}
    int getReturnValue(){return returnValue;}

   StackFrame() : mVars(), mExprs(), mPC() {
   }

   void bindDecl(Decl* decl, int val) {
      mVars[decl] = val;
   }    
   int getDeclVal(Decl * decl) {
      assert (mVars.find(decl) != mVars.end());
      return mVars.find(decl)->second;
   }
   void bindStmt(Stmt * stmt, int val) {
	   mExprs[stmt] = val;
   }
   int getStmtVal(Stmt * stmt) {
	   assert (mExprs.find(stmt) != mExprs.end());
	   return mExprs[stmt];
   }
   void setPC(Stmt * stmt) {
	   mPC = stmt;
   }
   Stmt * getPC() {
	   return mPC;
   }
};

/// Heap maps address to a value
/*
class Heap {
public:
   int Malloc(int size) ;
   void Free (int addr) ;
   void Update(int addr, int val) ;
   int get(int addr);
};
*/

class Environment {
public:

    std::vector<StackFrame> mStack;


    FunctionDecl * mFree;				/// Declartions to the built-in functions
   FunctionDecl * mMalloc;
   FunctionDecl * mInput;
   FunctionDecl * mOutput;

   FunctionDecl * mEntry;
   /// Get the declartions to the built-in functions
   Environment() : mStack(), mFree(NULL), mMalloc(NULL), mInput(NULL), mOutput(NULL), mEntry(NULL) {
   }


   /// Initialize the Environment
   void init(TranslationUnitDecl * unit) {
	   for (TranslationUnitDecl::decl_iterator i =unit->decls_begin(), e = unit->decls_end(); i != e; ++ i) {
		   if (FunctionDecl * fdecl = dyn_cast<FunctionDecl>(*i) ) {
			   if (fdecl->getName().equals("FREE")) mFree = fdecl;
			   else if (fdecl->getName().equals("MALLOC")) mMalloc = fdecl;
			   else if (fdecl->getName().equals("GET")) mInput = fdecl;
			   else if (fdecl->getName().equals("PRINT")) mOutput = fdecl;
			   else if (fdecl->getName().equals("main")) mEntry = fdecl;
		   }
	   }
       // 这里应该是 push 进一个main函数，一个函数对应一个 stack
	   mStack.push_back(StackFrame());
   }

   FunctionDecl * getEntry() {
	   return mEntry;
   }

    int getExprValue(Expr *expr) {
        if (IntegerLiteral *literal = dyn_cast<IntegerLiteral>(expr)) {
            // 整数常量值
            return literal->getValue().getSExtValue(); // class APIntStorage
        } else {
            return mStack.back().getStmtVal(expr);
        }
    }

    // 通过 retStmt 之前 bind过的 StmtVal 来获取具体的返回值
    void retstmt(ReturnStmt * ret){
       mStack.back().setReturnValue(mStack.back().getStmtVal(ret->getRetValue()));
   }

    //  提前存入 integer 的变量
    void integer(IntegerLiteral *integer){
       mStack.back().bindStmt(integer, integer->getValue().getSExtValue());
   }

   /// test04.c 处理 a=-10 中的 -
   void unaop(UnaryOperator *uop){
       // 保存此一元表达式的值到栈帧
       // 确定操作类型
       int result = 0;
       switch (uop->getOpcode()) {
           case UO_Minus:
               int value = mStack.back().getStmtVal(uop->getSubExpr());
               result = -value;
               break;

       }
       mStack.back().bindStmt(uop, result);

   }

    /// !TODO Support comparison operation
   void binop(BinaryOperator *bop) {
	   Expr * left = bop->getLHS();
	   Expr * right = bop->getRHS();

	   if (bop->isAssignmentOp()) {
           // 36 行的 assert 是从 map 里取不到值，应该需要先执行 bind
           // 应该不能手动 bind ，先 debug 过一遍流程吧
           // 需要在执行 bop 前，把 val 写进 mEnv 的 Stack
           // int val = getExprValue(right);
		   int val = mStack.back().getStmtVal(right);
		   mStack.back().bindStmt(left, val);
		   if (DeclRefExpr * declexpr = dyn_cast<DeclRefExpr>(left)) {
			   Decl * decl = declexpr->getFoundDecl();
			   mStack.back().bindDecl(decl, val);
		   }
	   } else if(bop->isAdditiveOp() || bop->isMultiplicativeOp() || bop->isComparisonOp()){
           int val1 = mStack.back().getStmtVal(left);
           int val2 = mStack.back().getStmtVal(right);
           int result;
           switch (bop->getOpcode()) {
               case BO_Add:
                   /// val1 is base, val2 is offset
                   if (left->getType()->isPointerType()) {
                       int base = val1 % 10000;
                       int offset = val1 / 10000 + val2;
                       result = base + offset * 10000;
                   }
                       /// val2 is base, val1 is offset
                   else if (right->getType()->isPointerType()) {
                       int base = val2 % 10000;
                       int offset = val2 / 10000 + val1;
                       result = base + offset * 10000;
                   } else {
                       result = val1 + val2;
                   }
                   break;
               case BO_Sub:
                   /// val1 is base, val2 is offset
                   if (left->getType()->isPointerType()) {
                       int base = val1 % 10000;
                       int offset = val1 / 10000 - val2;
                       result = base + offset * 10000;
                   }
                       /// val2 is base, val1 is offset
                   else if (right->getType()->isPointerType()) {
                       int base = val2 % 10000;
                       int offset = val2 / 10000 - val1;
                       result = base + offset * 10000;
                   } else {
                       result = val1 - val2;
                   }
                   break;
               case BO_Mul:
                   result = val1 * val2;
                   break;
               case BO_Div:
                   result = val1 / val2;
                   break;
               case BO_Rem:
                   result = val1 % val2;
                   break;
               case BO_GE:
                   result = val1 >= val2;
                   break;
               case BO_GT:
                   result = val1 > val2;
                   break;
               case BO_LE:
                   result = val1 <= val2;
                   break;
               case BO_LT:
                   result = val1 < val2;
                   break;
               case BO_EQ:
                   result = val1 == val2;
                   break;
               case BO_NE:
                   result = val1 != val2;
                   break;
               default:
                   throw std::exception();
                   break;
           }
           mStack.back().bindStmt(bop, result);
       }
   }


   void decl(DeclStmt * declstmt) {
	   for (DeclStmt::decl_iterator it = declstmt->decl_begin(), ie = declstmt->decl_end();
			   it != ie; ++ it) {
		   Decl * decl = *it;
		   if (VarDecl * vardecl = dyn_cast<VarDecl>(decl)) {
               /// test03.c 这里因为 val 一直是0，所以会取得错误结果，需要改
               // 这里需要改
               QualType type = vardecl->getType();
               if (type->isIntegerType()) {
                   IntegerLiteral *integer;
                   if (vardecl->hasInit() && (integer = dyn_cast<IntegerLiteral>(vardecl->getInit())))
                       mStack.back().bindDecl(vardecl, integer->getValue().getSExtValue());
                   else
                       mStack.back().bindDecl(vardecl, 0);
               }
		   }
	   }
   }

   /// test01.c 这里会 assert 报错
   void declref(DeclRefExpr * declref) {
	   mStack.back().setPC(declref);
	   if (declref->getType()->isIntegerType()) {
		   Decl* decl = declref->getFoundDecl();

		   int val = mStack.back().getDeclVal(decl);
		   mStack.back().bindStmt(declref, val);
	   }
   }

   void cast(CastExpr * castexpr) {
	   mStack.back().setPC(castexpr);
	   if (castexpr->getType()->isIntegerType()) {
		   Expr * expr = castexpr->getSubExpr();
		   int val = mStack.back().getStmtVal(expr);
		   mStack.back().bindStmt(castexpr, val );
	   }
   }

   /// !TODO Support Function Call
   void call(CallExpr * callexpr) {
	   mStack.back().setPC(callexpr);
	   int val = 0;
	   FunctionDecl * callee = callexpr->getDirectCallee();
	   if (callee == mInput) {
		  llvm::errs() << "Please Input an Integer Value : ";
		  scanf("%d", &val);

		  mStack.back().bindStmt(callexpr, val);
	   } else if (callee == mOutput) {
		   Expr * decl = callexpr->getArg(0);
		   val = mStack.back().getStmtVal(decl);
		   llvm::errs() << val;
	   } else {
		   /// You could add your code here for Function call Return
		   // 不是 buildin 的函数

	   }
   }
};


