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

#define HEAP_ROW_SIZE 10

class StackFrame {
    /// StackFrame maps Variable Declaration to Value
    /// Which are either integer or addresses (also represented using an Integer value)
    std::map<Decl *, int64_t> mVars;
    std::map<Stmt *, int64_t> mExprs;

    /// 存放指针，pair 的左边是地址，右边是具体的值
    std::map<Stmt *, std::pair<int64_t, int64_t>> mPtrs;
    /// The current stmt
    Stmt *mPC;
    int returnValue;


public:
    void setReturnValue(int ret) { returnValue = ret; }
    int getReturnValue() { return returnValue; }

    bool hasStmt(Stmt * stmt) {
        return (mExprs.find(stmt) != mExprs.end());
    }

    bool hasDecl(Decl* decl) {
        return (mVars.find(decl) != mVars.end());
    }

    StackFrame() : mVars(), mExprs(), mPC() {
    }

    void bindDecl(Decl *decl, int64_t val) {
        mVars[decl] = val;
    }

    void bindPtr(Stmt* stmt, int64_t addr, int64_t val){
        mPtrs[stmt]=  std::make_pair(addr,val);
    }

    /// 获取指针地址
    int64_t getPtrAddr(Stmt* stmt){
        return mPtrs[stmt].first;
    }

    int64_t getDeclVal(Decl *decl) {
        assert(mVars.find(decl) != mVars.end());
        return mVars.find(decl)->second;
    }
    void bindStmt(Stmt *stmt, int64_t val) {
        mExprs[stmt] = val;
    }
    int64_t getStmtVal(Stmt *stmt) {
        assert(mExprs.find(stmt) != mExprs.end());
        return mExprs[stmt];
    }

    void setPC(Stmt *stmt) {
        mPC = stmt;
    }
    Stmt *getPC() {
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
    // 堆，变长数组也可以做
    // 不能用二维的了，必须一个数组固定长度，比如*a 是100 *(a+1)是101
    std::vector<int64_t> mHeap;

    // 存放全局变量
    std::map<Decl*, int64_t> gVars;

    int depth = 0;

    FunctionDecl *mFree;/// Declartions to the built-in functions
    FunctionDecl *mMalloc;
    FunctionDecl *mInput;
    FunctionDecl *mOutput;

    FunctionDecl *mEntry;
    /// Get the declartions to the built-in functions
    Environment() : mStack(), mFree(NULL), mMalloc(NULL), mInput(NULL), mOutput(NULL), mEntry(NULL) {
        mStack.push_back(StackFrame()); // 初始栈帧，用于临时存储计算的全局变量值
    }


    /// Initialize the Environment
    void init(TranslationUnitDecl *unit) {
        for (TranslationUnitDecl::decl_iterator i = unit->decls_begin(), e = unit->decls_end(); i != e; ++i) {
            if (FunctionDecl *fdecl = dyn_cast<FunctionDecl>(*i)) {
                if (fdecl->getName().equals("FREE")) mFree = fdecl;
                else if (fdecl->getName().equals("MALLOC"))
                    mMalloc = fdecl;
                else if (fdecl->getName().equals("GET"))
                    mInput = fdecl;
                else if (fdecl->getName().equals("PRINT"))
                    mOutput = fdecl;
                else if (fdecl->getName().equals("main"))
                    mEntry = fdecl;
            }else if (VarDecl *vdecl = dyn_cast<VarDecl>(*i)) {
                // 保存全局变量
                Stmt * initStmt = vdecl->getInit();
                if (mStack.back().hasStmt(initStmt)) {
                    gVars[vdecl] = mStack.back().getStmtVal(initStmt);
                } else {
                    gVars[vdecl] = 0; // 未初始化的全局变量默认为 0
                }
            }
        }
        // 这里应该是 push 进一个main函数，一个函数对应一个 stack
        mStack.pop_back();
        mStack.push_back(StackFrame());
        depth++;
    }

    FunctionDecl *getEntry() {
        return mEntry;
    }

    int getExprValue(Expr *expr) {
        if (IntegerLiteral *literal = dyn_cast<IntegerLiteral>(expr)) {
            // 整数常量值
            return literal->getValue().getSExtValue();// class APIntStorage
        } else {
            return mStack.back().getStmtVal(expr);
        }
    }

    // 通过 retStmt 之前 bind过的 StmtVal 来获取具体的返回值
    void retstmt(ReturnStmt *ret) {
        mStack.back().setReturnValue(mStack.back().getStmtVal(ret->getRetValue()));
    }

    //  提前存入 integer 的变量
    void integer(IntegerLiteral *integer) {
        mStack.back().bindStmt(integer, integer->getValue().getSExtValue());
    }

    void array(ArraySubscriptExpr *arrExpr) {
        // 获取Base和Inx
        int64_t vIdx = mStack.back().getStmtVal(arrExpr->getBase());
        auto aIdx = mStack.back().getStmtVal(arrExpr->getIdx());

        int64_t val = mHeap[vIdx + aIdx];
        mStack.back().bindStmt(arrExpr, val);
    }

    /// test04.c 处理 a=-10 中的 -
    void unaop(UnaryOperator *uop) {
        // 保存此一元表达式的值到栈帧
        // 确定操作类型
        int result = 0;
        switch (uop->getOpcode()) {
            case UO_Minus: {
                int value = mStack.back().getStmtVal(uop->getSubExpr());
                result = -value;
                break;
            }
            case UO_Deref: {
                /// test18.c 需要考虑双重指针
                int64_t vIdx = mStack.back().getStmtVal(uop->getSubExpr());
                // 分别是地址和值
                mStack.back().bindPtr(uop, vIdx, mHeap[vIdx]);

                result = mHeap[vIdx];// 从堆数据结构中读取值
                // result = vIdx;
                break;
            }
        }
        mStack.back().bindStmt(uop, result);
    }

    /// test18.c 新增括号的支持
    void paren(ParenExpr *parenexpr) {
        mStack.back().bindStmt(parenexpr, mStack.back().getStmtVal(parenexpr->getSubExpr()));
    }

    /// test17.c support sizeof
    void uette(UnaryExprOrTypeTraitExpr *uettop) {
        int result = 0;
        // 同 uop，使用 switch 处理
        switch (uettop->getKind()) {
            case clang::UETT_SizeOf:
                // 没有 ASTContext，就按8来
                result = 8;
                break;
        }
        mStack.back().bindStmt(uettop, result);
    }

    /// test18.c 在第六个 binop 处报错
    void binop(BinaryOperator *bop) {
        Expr *left = bop->getLHS();
        Expr *right = bop->getRHS();
        int64_t result = 0;// 保存当前二元表达式的计算结果

        int64_t leftValue = mStack.back().getStmtVal(left);
        // right->dump();
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
                    mHeap[vIdx+aIdx] = val;
                } else {
                    throw std::exception();
                }
                return;
            } else if(llvm::isa<UnaryOperator>(left)){ // 左边的语句是单目运算符
                /// 左边是解引用，说明是指针
                if (llvm::dyn_cast<UnaryOperator>(left)->getOpcode() == UO_Deref){
                    // 获取ptr
                    int64_t addr = mStack.back().getPtrAddr(left);
                    mHeap[addr] = rightValue;
                    return ;
                }
            }
            mStack.back().bindStmt(left, val);
            if (DeclRefExpr *declexpr = dyn_cast<DeclRefExpr>(left)) {
                Decl *decl = declexpr->getFoundDecl();
                mStack.back().bindDecl(decl, val);
            }
        } else if (bop->isAdditiveOp() || bop->isMultiplicativeOp() || bop->isComparisonOp()) {
            int val1 = mStack.back().getStmtVal(left);
            int val2 = mStack.back().getStmtVal(right);
            switch (bop->getOpcode()) {
                case BO_Add:
                    /// val1 is base, val2 is offset
                    if (left->getType()->isPointerType() || right->getType()->isPointerType()) {
                        result = val1 + val2;
                    } else {
                        result = val1 + val2;
                    }
                    break;
                case BO_Sub:
                        result = val1 - val2;
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


    void decl(DeclStmt *declstmt) {
        for (DeclStmt::decl_iterator it = declstmt->decl_begin(), ie = declstmt->decl_end();
             it != ie; ++it) {
            Decl *decl = *it;
            if (VarDecl *vardecl = dyn_cast<VarDecl>(decl)) {
                /// test03.c 这里因为 val 一直是0，所以会取得错误结果，需要改
                // 这里需要改
                QualType type = vardecl->getType();
                if (type->isIntegerType()) {
                    IntegerLiteral *integer;
                    if (vardecl->hasInit() && (integer = dyn_cast<IntegerLiteral>(vardecl->getInit())))
                        mStack.back().bindDecl(vardecl, integer->getValue().getSExtValue());
                    else
                        mStack.back().bindDecl(vardecl, 0);
                } else if (type->isArrayType()) {
                    // 获取数组类型并确定其大小
                    const ConstantArrayType *arrayType = dyn_cast<ConstantArrayType>(type.getTypePtr());
                    int arraySize = arrayType->getSize().getSExtValue();

                    // 创建一个数组来保存初始值或默认值
                    // 这个数组指针是用来存放在 bindDecl 里的
                    // 这里必须 malloc 让他一直在内存里泄漏，要不然作用域过去了就没了
                    //int64_t* arrayValues = (int64_t *)malloc(arraySize*sizeof(int64_t));  // 默认初始化为0

                    // std::vector<int64_t> arrayValues(arraySize, 0);
                    // 固定为100个，不能再分 vIdx和aIdx了
                    std::vector<int64_t> arrayValues(HEAP_ROW_SIZE, 0);
                    if (vardecl->hasInit()) {
                        // 如果有初始值，则提取并存储
                        const InitListExpr *initList = dyn_cast<InitListExpr>(vardecl->getInit());
                        for (int i = 0; i < initList->getNumInits() && i < arraySize; i++) {
                            const IntegerLiteral *initValue = dyn_cast<IntegerLiteral>(initList->getInit(i));
                            arrayValues[i] = initValue->getValue().getSExtValue();
                        }
                    } else {// 没有初识值，全写0
                        // 切换到vector之后就不用操作了
                    }

                    // 将数组的值与声明绑定
                    /// test12.c 直接存放地址会报错，还是用一个全局的堆
                    ///mStack.back().bindDecl(vardecl, (int64_t)arrayValues);
                    // 存放数组，并且获取对应的idx
                    arrayValues.resize(HEAP_ROW_SIZE, 0);
                    mHeap.insert(mHeap.begin(), arrayValues.begin(), arrayValues.end());
                    int64_t idx = mHeap.size() - HEAP_ROW_SIZE;
                    mStack.back().bindDecl(vardecl, idx);
                } else if (type->isPointerType()) {
                    std::vector<int64_t> arrayValues(HEAP_ROW_SIZE, 0);
                    mHeap.insert(mHeap.begin(), arrayValues.begin(), arrayValues.end());
                    int64_t idx = mHeap.size() - HEAP_ROW_SIZE;
                    mStack.back().bindDecl(vardecl, idx);
                } else {
                    declstmt->dump();
                }
            }
        }
    }

    /// test01.c 这里会 assert 报错
    /// test17.c 这里需要支持 malloc 函数
    /// test22-24.c 啥都没干就过了，输出都是2442，对吗，24不对

    void declref(DeclRefExpr *declref) {
        mStack.back().setPC(declref);
        if (declref->getType()->isIntegerType()) {
            Decl *decl = declref->getFoundDecl();
            /// test1.c 添加全局变量的支持
            int val;
            if (mStack.back().hasDecl(decl)) {
                val = mStack.back().getDeclVal(decl);
            } else {
                assert (gVars.find(decl) != gVars.end());
                val = gVars[decl];
            }
            mStack.back().bindStmt(declref, val);
        } else if (declref->getType()->isArrayType()) {/// test12.c 需要支持数组
            Decl *decl = declref->getFoundDecl();
            /// test12.c 这一句会报错，猜测是没有正确处理 decl 里面的数组定义
            int val;
            if (mStack.back().hasDecl(decl)) {
                val = mStack.back().getDeclVal(decl);
            } else {
                assert (gVars.find(decl) != gVars.end());
                val = gVars[decl];
            }
            mStack.back().bindStmt(declref, val);
        } else if (declref->getType()->isPointerType()
                   //&& declref->getType()->getAs<PointerType>()->getPointeeType()->isFunctionType()
                   ) {/// test14.c 需要支持数组
            Decl *decl = declref->getFoundDecl();

            int val;
            if (mStack.back().hasDecl(decl)) {
                val = mStack.back().getDeclVal(decl);
            } else {
                assert (gVars.find(decl) != gVars.end());
                val = gVars[decl];
            }
            mStack.back().bindStmt(declref, val);
        } else if (declref->getType()->isFunctionType()) {
         //   Decl *decl = declref->getFoundDecl();

            /// test17.c 为什么这里找不到 decl ？？？？
         //   llvm::errs() << "funcs ";
         //   int64_t val = mStack.back().getDeclVal(decl);
         //   mStack.back().bindStmt(declref, val);
        }else {
                declref->dump();

        }
    }

    void cast(CastExpr *castexpr) {
        mStack.back().setPC(castexpr);
        if ((castexpr->getType()->isIntegerType()) || (castexpr->getCastKind() == CK_ArrayToPointerDecay) || castexpr->getCastKind() == CK_BitCast) {
            /// test17.c 这里的 expr 是单目运算符 sizeof
            Expr *expr = castexpr->getSubExpr();
            int val = mStack.back().getStmtVal(expr);
            mStack.back().bindStmt(castexpr, val);
        } else if (castexpr->getType()->isPointerType() && castexpr->getCastKind() != CK_FunctionToPointerDecay) {
            /// 一定要排除掉 FunctionToPointerDecaty
            /// test17.c 这里的 expr 是单目运算符 sizeof
            Expr *expr = castexpr->getSubExpr();
            int val = mStack.back().getStmtVal(expr);
            mStack.back().bindStmt(castexpr, val);
        } else {
            // FunctoPoninterDecay 不用管
            if(castexpr->getCastKind() != CK_FunctionToPointerDecay)
                castexpr->dump();
        }
    }

    /// !TODO Support Function Call
    void call(CallExpr *callexpr) {
        mStack.back().setPC(callexpr);
        int val = 0;
        FunctionDecl *callee = callexpr->getDirectCallee();
        if (callee == mInput) {
            llvm::errs() << "Please Input an Integer Value : ";
            scanf("%d", &val);

            mStack.back().bindStmt(callexpr, val);
        } else if (callee == mOutput) {
            Expr *decl = callexpr->getArg(0);
            val = mStack.back().getStmtVal(decl);
            llvm::errs() << val;
        } else if (callee == mMalloc) {/// test17.c 需要实现 MALLOC FREE.
            int numArgs = callexpr->getNumArgs();
            int arg;
            assert(numArgs == 1);
            arg = mStack.back().getStmtVal(callexpr->getArg(0));
            // 分配对应的内存
            std::vector<int64_t> mallocRes(HEAP_ROW_SIZE, 0);

            mHeap.insert(mHeap.begin(),mallocRes.begin(),mallocRes.end());
            // 存放基地址
            int vIdx = mHeap.size() - HEAP_ROW_SIZE;
            mStack.back().bindStmt(callexpr, vIdx);
        } else if (callee == mFree) {
            // 不用操作也没事吧
        } else {
            // 非内建函数不应该出现在这里
            callexpr->dump();
            // exit(0);
        }
    }
};
