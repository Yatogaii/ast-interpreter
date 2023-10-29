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
   std::map<Decl*, int64_t> mVars;
   std::map<Stmt*, int64_t> mExprs;
   /// The current stmt
   Stmt * mPC;
   int returnValue;
public:
    void setReturnValue(int ret){ returnValue=ret;}
    int getReturnValue(){return returnValue;}

   StackFrame() : mVars(), mExprs(), mPC() {
   }

   void bindDecl(Decl* decl, int64_t val) {
      mVars[decl] = val;
   }    
   int64_t getDeclVal(Decl * decl) {
      assert (mVars.find(decl) != mVars.end());
      return mVars.find(decl)->second;
   }
   void bindStmt(Stmt * stmt, int64_t val) {
	   mExprs[stmt] = val;
   }
   int64_t getStmtVal(Stmt * stmt) {
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

//class Heap{
//public:
//    std::vector<int64_t> datas;
//    int curBaseAddr = 0;
//};

class Environment {
public:

    std::vector<StackFrame> mStack;
    // 堆，先不考虑变长数组
    std::vector<std::vector<int64_t>> mHeap;


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

   void array(ArraySubscriptExpr* arrExpr){
       // 获取Base和Inx
       int64_t vIdx = mStack.back().getStmtVal(arrExpr->getBase());
       auto aIdx = mStack.back().getStmtVal(arrExpr->getIdx());

       int64_t val = mHeap[vIdx][aIdx];
       mStack.back().bindStmt(arrExpr, val);
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
        int64_t result = 0; // 保存当前二元表达式的计算结果

        // 赋值运算：=, *=, /=, %=, +=, -=, ...
        // 算数和逻辑运算：+, -, *, /, %, <<, >>, &, ^, |

        int64_t leftValue = mStack.back().getStmtVal(left);
        int64_t rightValue = mStack.back().getStmtVal(right);

	   if (bop->isAssignmentOp()) {
           // 36 行的 assert 是从 map 里取不到值，应该需要先执行 bind
           // 应该不能手动 bind ，先 debug 过一遍流程吧
           // 需要在执行 bop 前，把 val 写进 mEnv 的 Stack
		   //int val = mStack.back().getStmtVal(right);
		   //mStack.back().bindStmt(left, val);
		   //if (DeclRefExpr * declexpr = dyn_cast<DeclRefExpr>(left)) {
			//   Decl * decl = declexpr->getFoundDecl();
			//   mStack.back().bindDecl(decl, val);
		   //}

           int val = mStack.back().getStmtVal(right);
           /// 目前为止只有数组和指针能做左值
           if (llvm::isa<ArraySubscriptExpr>(left)) {
               ArraySubscriptExpr *array = llvm::dyn_cast<ArraySubscriptExpr>(left);
               int vIdx = mStack.back().getStmtVal(array->getBase());
               int aIdx = mStack.back().getStmtVal(array->getIdx());
               if (array->getType()->isIntegerType() || array->getType()->isPointerType()) {
                   mHeap[vIdx][aIdx] = val;
               } else {
                   throw std::exception();
               }
               return;
           }
           mStack.back().bindStmt(left, val);
           if (DeclRefExpr *declexpr = dyn_cast<DeclRefExpr>(left)) {
               Decl *decl = declexpr->getFoundDecl();
               mStack.back().bindDecl(decl, val);
           }
	   } else if(bop->isAdditiveOp() || bop->isMultiplicativeOp() || bop->isComparisonOp()){
           int val1 = mStack.back().getStmtVal(left);
           int val2 = mStack.back().getStmtVal(right);
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
               } else if(type->isArrayType()){
                   // 获取数组类型并确定其大小
                   const ConstantArrayType *arrayType = dyn_cast<ConstantArrayType>(type.getTypePtr());
                   int arraySize = arrayType->getSize().getSExtValue();

                   // 创建一个数组来保存初始值或默认值
                   // 这个数组指针是用来存放在 bindDecl 里的
                   // 这里必须 malloc 让他一直在内存里泄漏，要不然作用域过去了就没了
                   //int64_t* arrayValues = (int64_t *)malloc(arraySize*sizeof(int64_t));  // 默认初始化为0

                   std::vector<int64_t> arrayValues(arraySize,0);
                   if (vardecl->hasInit()) {
                       // 如果有初始值，则提取并存储
                       const InitListExpr *initList = dyn_cast<InitListExpr>(vardecl->getInit());
                       for (int i = 0; i < initList->getNumInits() && i < arraySize; i++) {
                           const IntegerLiteral *initValue = dyn_cast<IntegerLiteral>(initList->getInit(i));
                           arrayValues[i] = initValue->getValue().getSExtValue();
                       }
                   } else { // 没有初识值，全写0
                       // 切换到vector之后就不用操作了
                   }

                   // 将数组的值与声明绑定
                   /// test12.c 直接存放地址会报错，还是用一个全局的堆
                   ///mStack.back().bindDecl(vardecl, (int64_t)arrayValues);
                   // 存放数组，并且获取对应的idx
                   mHeap.push_back(arrayValues);
                   int64_t idx = mHeap.size()-1;
                   mStack.back().bindDecl(vardecl, idx);
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
	   } else if (declref->getType()->isArrayType()){ /// test12.c 需要支持数组
           Decl* decl = declref->getFoundDecl();
            /// test12.c 这一句会报错，猜测是没有正确处理 decl 里面的数组定义
           int64_t idx = mStack.back().getDeclVal(decl);
           mStack.back().bindStmt(declref, idx);
       } else if (declref->getType()->isPointerType()){ /// test14.c 需要支持数组
           Decl* decl = declref->getFoundDecl();

           int64_t val = mStack.back().getDeclVal(decl);
           mStack.back().bindStmt(declref, val);
       }
   }

   void cast(CastExpr * castexpr) {
	   mStack.back().setPC(castexpr);
	   if ((castexpr->getType()->isIntegerType())
       || (castexpr->getCastKind() == CK_ArrayToPointerDecay)) {
		   Expr * expr = castexpr->getSubExpr();
		   int val = mStack.back().getStmtVal(expr);
		   mStack.back().bindStmt(castexpr, val );
	   } else {
           castexpr->dump();
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


