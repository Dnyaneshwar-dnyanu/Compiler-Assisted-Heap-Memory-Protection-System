#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/TypeSize.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <memory>

using namespace llvm;

namespace {

/**
 * @brief Classification of memory pointer origins.
 */
enum class PointerOrigin {
    HEAP_CANDIDATE,
    NON_HEAP,
    UNKNOWN
};

static StringRef originToString(PointerOrigin Origin) {
    switch (Origin) {
    case PointerOrigin::HEAP_CANDIDATE:
        return "HEAP_CANDIDATE";
    case PointerOrigin::NON_HEAP:
        return "NON_HEAP";
    case PointerOrigin::UNKNOWN:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

/**
 * @brief Checks if a Value represents a direct call to standard heap allocation APIs.
 *
 * Requires both the callsite and the called function to use the standard C calling convention.
 */
static bool isHeapAllocCall(const Value *V) {
    auto *CB = dyn_cast<CallBase>(V);
    if (!CB) {
        return false;
    }
    Function *Callee = CB->getCalledFunction();
    if (!Callee || Callee->isIntrinsic()) {
        return false;
    }
    if (!CB->getType()->isPointerTy()) {
        return false;
    }
    // Require both the callsite and the called function to use the C calling convention
    if (Callee->getCallingConv() != CallingConv::C || CB->getCallingConv() != CallingConv::C) {
        return false;
    }

    StringRef Name = Callee->getName();
    if ((Name == "malloc" || Name == "mg_malloc" || Name == "mg_intercept_malloc") &&
        CB->arg_size() == 1) {
        return true;
    }
    if ((Name == "calloc" || Name == "mg_intercept_calloc") && CB->arg_size() == 2) {
        return true;
    }
    if ((Name == "realloc" || Name == "mg_intercept_realloc") && CB->arg_size() == 2) {
        return true;
    }
    return false;
}

/**
 * @brief Checks if a function belongs to the MemoryGuard runtime.
 *
 * Calls within MemoryGuard runtime functions must not be rewritten or instrumented
 * to prevent infinite recursion. Matches exact runtime API names to avoid excluding
 * unrelated application functions beginning with "mg_".
 */
static bool isMemoryGuardRuntimeFunction(const Function &F) {
    StringRef Name = F.getName();
    return Name == "mg_malloc" ||
           Name == "mg_free" ||
           Name == "mg_is_tracked" ||
           Name == "mg_get_allocation_size" ||
           Name == "mg_get_tracked_count" ||
           Name == "mg_is_valid_access" ||
           Name == "mg_check_access" ||
           Name == "mg_register_external" ||
           Name == "mg_unregister_external" ||
           Name == "mg_realloc_external" ||
           Name == "mg_intercept_malloc" ||
           Name == "mg_intercept_calloc" ||
           Name == "mg_intercept_realloc" ||
           Name == "mg_intercept_free";
}

/**
 * @brief Analysis context for interprocedural pointer-origin classification.
 *
 * Holds memoized argument classifications and cached DominatorTrees for callers
 * to avoid duplicate call-site traversals and dominator recalculations.
 */
struct InterproceduralContext {
    DenseMap<const Argument *, PointerOrigin> ArgCache;
    DenseMap<const Function *, std::unique_ptr<DominatorTree>> DTCache;
    const DominatorTree *CurrentDT = nullptr;
};

/**
 * @brief RAII helper for recursion-path-based cycle detection.
 *
 * Adds the value to the visited set upon entry and removes it upon scope exit,
 * ensuring independent branches (e.g. SelectInst arms, PHI incoming edges, multiple stores)
 * do not interfere with each other's visited state while preventing cyclic recursion.
 */
class PathVisitedScope {
    SmallPtrSetImpl<const Value *> &Visited;
    const Value *Val;
    bool Inserted;

public:
    PathVisitedScope(SmallPtrSetImpl<const Value *> &Set, const Value *V)
        : Visited(Set), Val(V), Inserted(false) {
        if (Val) {
            Inserted = Visited.insert(Val).second;
        }
    }

    ~PathVisitedScope() {
        if (Inserted && Val) {
            Visited.erase(Val);
        }
    }

    bool isCycle() const {
        return !Inserted;
    }
};

// Forward declarations
static constexpr unsigned MaxTraversalDepth = 32;

static PointerOrigin tracePointerOrigin(const Value *V,
                                        const DominatorTree *DT,
                                        SmallPtrSetImpl<const Value *> &Visited,
                                        unsigned Depth,
                                        InterproceduralContext &ICtx);

/**
 * @brief Interprocedural pointer-origin analysis for function parameters.
 *
 * Examines all direct call sites of the enclosing function:
 *  - Requires the function to have local linkage (static in C) and address not taken,
 *    ensuring all callers in the program reside within the current LLVM module.
 *  - Requires all call sites to establish the same non-UNKNOWN origin (e.g. HEAP_CANDIDATE).
 *  - If call-site information is incomplete, mixed, recursive, or ambiguous,
 *    conservatively returns UNKNOWN.
 */
static PointerOrigin traceArgumentOrigin(const Argument *Arg,
                                         SmallPtrSetImpl<const Value *> &Visited,
                                         unsigned Depth,
                                         InterproceduralContext &ICtx) {
    if (!Arg || !Arg->getType()->isPointerTy()) {
        return PointerOrigin::UNKNOWN;
    }

    // Check memoization cache first
    auto CacheIt = ICtx.ArgCache.find(Arg);
    if (CacheIt != ICtx.ArgCache.end()) {
        return CacheIt->second;
    }

    const Function *F = Arg->getParent();
    if (!F || F->isDeclaration()) {
        return PointerOrigin::UNKNOWN;
    }

    // 1. Must have local linkage to guarantee all possible callers are within this module.
    if (!F->hasLocalLinkage()) {
        ICtx.ArgCache[Arg] = PointerOrigin::UNKNOWN;
        return PointerOrigin::UNKNOWN;
    }

    // 2. Function address must not be taken (cannot be called indirectly).
    if (F->hasAddressTaken()) {
        ICtx.ArgCache[Arg] = PointerOrigin::UNKNOWN;
        return PointerOrigin::UNKNOWN;
    }

    // 3. Must have at least one caller in the module.
    if (F->use_empty()) {
        ICtx.ArgCache[Arg] = PointerOrigin::UNKNOWN;
        return PointerOrigin::UNKNOWN;
    }

    unsigned ArgNo = Arg->getArgNo();
    PointerOrigin CommonOrigin = PointerOrigin::UNKNOWN;
    bool FirstCallSite = true;

    // 4. Examine all call sites of F in the module
    for (const Use &U : F->uses()) {
        const User *UserObj = U.getUser();
        auto *CB = dyn_cast<CallBase>(UserObj);
        if (!CB) {
            // Function is used in something other than a direct call instruction
            ICtx.ArgCache[Arg] = PointerOrigin::UNKNOWN;
            return PointerOrigin::UNKNOWN;
        }

        // Verify F is the called callee, not an argument to another call
        if (!CB->isCallee(&U)) {
            ICtx.ArgCache[Arg] = PointerOrigin::UNKNOWN;
            return PointerOrigin::UNKNOWN;
        }

        // Argument index must be within range of call site operands
        if (ArgNo >= CB->arg_size()) {
            ICtx.ArgCache[Arg] = PointerOrigin::UNKNOWN;
            return PointerOrigin::UNKNOWN;
        }

        Value *ActualArg = CB->getArgOperand(ArgNo);
        if (!ActualArg || !ActualArg->getType()->isPointerTy()) {
            ICtx.ArgCache[Arg] = PointerOrigin::UNKNOWN;
            return PointerOrigin::UNKNOWN;
        }

        const Function *Caller = CB->getFunction();
        if (!Caller || Caller->isDeclaration()) {
            ICtx.ArgCache[Arg] = PointerOrigin::UNKNOWN;
            return PointerOrigin::UNKNOWN;
        }

        if (isMemoryGuardRuntimeFunction(*Caller)) {
            ICtx.ArgCache[Arg] = PointerOrigin::UNKNOWN;
            return PointerOrigin::UNKNOWN;
        }

        // Obtain or compute the DominatorTree for Caller
        const DominatorTree *CallerDT = nullptr;
        if (ICtx.CurrentDT && ICtx.CurrentDT->getRoot() &&
            ICtx.CurrentDT->getRoot()->getParent() == Caller) {
            CallerDT = ICtx.CurrentDT;
        } else {
            auto DtIt = ICtx.DTCache.find(Caller);
            if (DtIt != ICtx.DTCache.end()) {
                CallerDT = DtIt->second.get();
            } else {
                auto NewDT = std::make_unique<DominatorTree>(const_cast<Function &>(*Caller));
                CallerDT = NewDT.get();
                ICtx.DTCache[Caller] = std::move(NewDT);
            }
        }

        // Trace the origin of ActualArg in the caller's context
        PointerOrigin ActualOrigin = tracePointerOrigin(ActualArg, CallerDT, Visited, Depth + 1, ICtx);

        if (ActualOrigin == PointerOrigin::UNKNOWN) {
            ICtx.ArgCache[Arg] = PointerOrigin::UNKNOWN;
            return PointerOrigin::UNKNOWN;
        }

        if (FirstCallSite) {
            CommonOrigin = ActualOrigin;
            FirstCallSite = false;
        } else if (CommonOrigin != ActualOrigin) {
            // Inconsistent origins across call sites (e.g. heap vs non-heap)
            ICtx.ArgCache[Arg] = PointerOrigin::UNKNOWN;
            return PointerOrigin::UNKNOWN;
        }
    }

    ICtx.ArgCache[Arg] = CommonOrigin;
    return CommonOrigin;
}

/**
 * @brief Conservatively analyzes pointer values loaded from local memory (alloca).
 *
 * An alloca itself represents stack memory. However, if a pointer loaded from an
 * alloca is dereferenced, that loaded pointer may point to heap memory.
 *
 * Strict correctness requirements:
 *  - Only classify a pointer from a store when that store is proven to dominate the load.
 *  - Do not use fallbacks based on all stores in the function agreeing.
 *  - If no dominating store can be established, return UNKNOWN.
 */
static PointerOrigin traceAllocaOrigin(const AllocaInst *AI, const LoadInst *LI,
                                       const DominatorTree *DT,
                                       SmallPtrSetImpl<const Value *> &Visited,
                                       unsigned Depth,
                                       InterproceduralContext &ICtx) {
    // Collect all stores to AI and check if AI's address escapes
    SmallVector<const StoreInst *, 4> Stores;
    for (const User *U : AI->users()) {
        if (auto *SI = dyn_cast<StoreInst>(U)) {
            // If AI is stored as a value (e.g. storing &AI), it escapes
            if (SI->getValueOperand() == AI) {
                return PointerOrigin::UNKNOWN;
            }
            if (SI->getPointerOperand() == AI) {
                Stores.push_back(SI);
            }
        } else if (isa<LoadInst>(U)) {
            continue;
        } else if (auto *II = dyn_cast<IntrinsicInst>(U)) {
            // Lifetime markers and debug info intrinsics do not escape the address
            if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
                II->getIntrinsicID() == Intrinsic::lifetime_end ||
                isa<DbgInfoIntrinsic>(II)) {
                continue;
            }
            return PointerOrigin::UNKNOWN;
        } else {
            // Any other use means AI potentially escapes or has complex aliases
            return PointerOrigin::UNKNOWN;
        }
    }

    if (Stores.empty()) {
        return PointerOrigin::UNKNOWN;
    }

    // 1. Intra-basic-block dominating store:
    // If there is a store to AI in the same basic block preceding LI,
    // the most recent such store strictly dominates and reaches LI.
    const StoreInst *LocalDominatingStore = nullptr;
    for (const Instruction *I = LI->getPrevNode(); I != nullptr; I = I->getPrevNode()) {
        if (auto *SI = dyn_cast<StoreInst>(I)) {
            if (SI->getPointerOperand() == AI) {
                LocalDominatingStore = SI;
                break;
            }
        }
    }

    if (LocalDominatingStore) {
        return tracePointerOrigin(LocalDominatingStore->getValueOperand(), DT, Visited, Depth + 1, ICtx);
    }

    // 2. Inter-basic-block dominating store:
    // If there is exactly ONE store to AI in the entire function and that store
    // is proven by DominatorTree to dominate LI, it is guaranteed to reach LI.
    if (Stores.size() == 1 && DT) {
        const StoreInst *SingleStore = Stores[0];
        if (DT->dominates(SingleStore, LI)) {
            return tracePointerOrigin(SingleStore->getValueOperand(), DT, Visited, Depth + 1, ICtx);
        }
    }

    // If no store can be proven to dominate the load, return UNKNOWN conservatively.
    return PointerOrigin::UNKNOWN;
}

/**
 * @brief Recursive pointer-origin tracing with path-based cycle detection and depth limiting.
 */
static PointerOrigin tracePointerOrigin(const Value *V,
                                        const DominatorTree *DT,
                                        SmallPtrSetImpl<const Value *> &Visited,
                                        unsigned Depth,
                                        InterproceduralContext &ICtx) {
    if (!V) {
        return PointerOrigin::UNKNOWN;
    }

    // Depth limit to prevent long chains or analysis overhead
    if (Depth > MaxTraversalDepth) {
        return PointerOrigin::UNKNOWN;
    }

    // Recursion-path-based cycle detection with RAII cleanup on scope exit
    PathVisitedScope Scope(Visited, V);
    if (Scope.isCycle()) {
        return PointerOrigin::UNKNOWN;
    }

    // 1. Direct heap allocation API calls
    if (isHeapAllocCall(V)) {
        return PointerOrigin::HEAP_CANDIDATE;
    }

    // 2. Stack allocations (AllocaInst)
    if (isa<AllocaInst>(V)) {
        return PointerOrigin::NON_HEAP;
    }

    // 3. Global values (GlobalVariable, Function, etc.)
    if (isa<GlobalValue>(V)) {
        return PointerOrigin::NON_HEAP;
    }

    // 4. Null pointer constants
    if (isa<ConstantPointerNull>(V)) {
        return PointerOrigin::NON_HEAP;
    }

    // 5. Undefined or poison values
    if (isa<UndefValue>(V)) {
        return PointerOrigin::UNKNOWN;
    }

    // 6. Function arguments: interprocedural call-site origin analysis
    if (auto *Arg = dyn_cast<Argument>(V)) {
        return traceArgumentOrigin(Arg, Visited, Depth, ICtx);
    }

    // 7. GEP instructions: follow to source pointer operand
    if (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
        return tracePointerOrigin(GEP->getPointerOperand(), DT, Visited, Depth + 1, ICtx);
    }

    // 8. Pointer casts: follow safe pointer-preserving casts only.
    // Return UNKNOWN for PtrToInt, IntToPtr, or other unsafe conversions.
    if (auto *Cast = dyn_cast<CastInst>(V)) {
        if (isa<BitCastInst>(Cast) || isa<AddrSpaceCastInst>(Cast)) {
            return tracePointerOrigin(Cast->getOperand(0), DT, Visited, Depth + 1, ICtx);
        }
        return PointerOrigin::UNKNOWN;
    }

    // 9. Freeze instructions
    if (auto *FI = dyn_cast<FreezeInst>(V)) {
        return tracePointerOrigin(FI->getOperand(0), DT, Visited, Depth + 1, ICtx);
    }

    // 10. Constant expressions (GEP or safe pointer casts)
    if (auto *CE = dyn_cast<ConstantExpr>(V)) {
        if (CE->getOpcode() == Instruction::GetElementPtr ||
            CE->getOpcode() == Instruction::BitCast ||
            CE->getOpcode() == Instruction::AddrSpaceCast) {
            return tracePointerOrigin(CE->getOperand(0), DT, Visited, Depth + 1, ICtx);
        }
        return PointerOrigin::UNKNOWN;
    }

    // 11. Select instructions: evaluate both arms
    if (auto *SI = dyn_cast<SelectInst>(V)) {
        PointerOrigin TrueOrigin = tracePointerOrigin(SI->getTrueValue(), DT, Visited, Depth + 1, ICtx);
        PointerOrigin FalseOrigin = tracePointerOrigin(SI->getFalseValue(), DT, Visited, Depth + 1, ICtx);
        if (TrueOrigin == FalseOrigin && TrueOrigin != PointerOrigin::UNKNOWN) {
            return TrueOrigin;
        }
        return PointerOrigin::UNKNOWN;
    }

    // 12. PHI nodes: check all incoming values
    if (auto *PN = dyn_cast<PHINode>(V)) {
        if (PN->getNumIncomingValues() == 0) {
            return PointerOrigin::UNKNOWN;
        }
        PointerOrigin CommonOrigin = PointerOrigin::UNKNOWN;
        bool First = true;
        for (const Value *Incoming : PN->incoming_values()) {
            PointerOrigin IncomingOrigin = tracePointerOrigin(Incoming, DT, Visited, Depth + 1, ICtx);
            if (IncomingOrigin == PointerOrigin::UNKNOWN) {
                return PointerOrigin::UNKNOWN;
            }
            if (First) {
                CommonOrigin = IncomingOrigin;
                First = false;
            } else if (CommonOrigin != IncomingOrigin) {
                return PointerOrigin::UNKNOWN;
            }
        }
        return CommonOrigin;
    }

    // 13. Load instructions: conservative local memory tracking with proven dominance
    if (auto *LI = dyn_cast<LoadInst>(V)) {
        const Value *LoadSource = LI->getPointerOperand();
        if (auto *AI = dyn_cast<AllocaInst>(LoadSource)) {
            return traceAllocaOrigin(AI, LI, DT, Visited, Depth + 1, ICtx);
        }
        return PointerOrigin::UNKNOWN;
    }

    return PointerOrigin::UNKNOWN;
}

/**
 * @brief Public helper to classify a pointer's origin using interprocedural analysis context.
 */
static PointerOrigin classifyPointerOrigin(const Value *V,
                                           const DominatorTree *DT,
                                           InterproceduralContext &ICtx) {
    SmallPtrSet<const Value *, 16> Visited;
    return tracePointerOrigin(V, DT, Visited, 0, ICtx);
}

/**
 * @brief Public helper to classify a pointer's origin.
 */
[[maybe_unused]] static PointerOrigin classifyPointerOrigin(const Value *V, const DominatorTree *DT) {
    InterproceduralContext ICtx;
    ICtx.CurrentDT = DT;
    SmallPtrSet<const Value *, 16> Visited;
    return tracePointerOrigin(V, DT, Visited, 0, ICtx);
}

/**
 * @brief Retrieves or declares the mg_check_access function declaration in the target module.
 *
 * void mg_check_access(const void *ptr, size_t access_size);
 */
static FunctionCallee getOrInsertCheckFunction(Module &M, const DataLayout &DL) {
    LLVMContext &Ctx = M.getContext();
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *PtrTy = PointerType::getUnqual(Ctx);
    Type *SizeTy = DL.getIntPtrType(Ctx);
    FunctionType *FTy = FunctionType::get(VoidTy, {PtrTy, SizeTy}, false);
    return M.getOrInsertFunction("mg_check_access", FTy);
}

static FunctionCallee getOrInsertInterceptMalloc(Module &M, const DataLayout &DL) {
    LLVMContext &Ctx = M.getContext();
    Type *PtrTy = PointerType::getUnqual(Ctx);
    Type *SizeTy = DL.getIntPtrType(Ctx);
    FunctionType *FTy = FunctionType::get(PtrTy, {SizeTy}, false);
    return M.getOrInsertFunction("mg_intercept_malloc", FTy);
}

static FunctionCallee getOrInsertInterceptCalloc(Module &M, const DataLayout &DL) {
    LLVMContext &Ctx = M.getContext();
    Type *PtrTy = PointerType::getUnqual(Ctx);
    Type *SizeTy = DL.getIntPtrType(Ctx);
    FunctionType *FTy = FunctionType::get(PtrTy, {SizeTy, SizeTy}, false);
    return M.getOrInsertFunction("mg_intercept_calloc", FTy);
}

static FunctionCallee getOrInsertInterceptRealloc(Module &M, const DataLayout &DL) {
    LLVMContext &Ctx = M.getContext();
    Type *PtrTy = PointerType::getUnqual(Ctx);
    Type *SizeTy = DL.getIntPtrType(Ctx);
    FunctionType *FTy = FunctionType::get(PtrTy, {PtrTy, SizeTy}, false);
    return M.getOrInsertFunction("mg_intercept_realloc", FTy);
}

static FunctionCallee getOrInsertInterceptFree(Module &M) {
    LLVMContext &Ctx = M.getContext();
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *PtrTy = PointerType::getUnqual(Ctx);
    FunctionType *FTy = FunctionType::get(VoidTy, {PtrTy}, false);
    return M.getOrInsertFunction("mg_intercept_free", FTy);
}

/**
 * @brief Checks if a call instruction represents a compatible standard malloc call.
 */
static bool isCompatibleMallocCall(const CallBase *CB, const DataLayout &DL) {
    Function *Callee = CB->getCalledFunction();
    if (!Callee || Callee->isIntrinsic()) return false;
    if (Callee->getCallingConv() != CallingConv::C || CB->getCallingConv() != CallingConv::C) return false;
    if (Callee->getName() != "malloc") return false;
    if (CB->arg_size() != 1) return false;
    if (!CB->getType()->isPointerTy()) return false;
    Type *ArgTy = CB->getArgOperand(0)->getType();
    if (!ArgTy->isIntegerTy()) return false;
    Type *SizeTy = DL.getIntPtrType(CB->getContext());
    if (ArgTy != SizeTy) return false;
    return true;
}

/**
 * @brief Checks if a call instruction represents a compatible standard calloc call.
 */
static bool isCompatibleCallocCall(const CallBase *CB, const DataLayout &DL) {
    Function *Callee = CB->getCalledFunction();
    if (!Callee || Callee->isIntrinsic()) return false;
    if (Callee->getCallingConv() != CallingConv::C || CB->getCallingConv() != CallingConv::C) return false;
    if (Callee->getName() != "calloc") return false;
    if (CB->arg_size() != 2) return false;
    if (!CB->getType()->isPointerTy()) return false;
    Type *SizeTy = DL.getIntPtrType(CB->getContext());
    if (CB->getArgOperand(0)->getType() != SizeTy) return false;
    if (CB->getArgOperand(1)->getType() != SizeTy) return false;
    return true;
}

/**
 * @brief Checks if a call instruction represents a compatible standard realloc call.
 */
static bool isCompatibleReallocCall(const CallBase *CB, const DataLayout &DL) {
    Function *Callee = CB->getCalledFunction();
    if (!Callee || Callee->isIntrinsic()) return false;
    if (Callee->getCallingConv() != CallingConv::C || CB->getCallingConv() != CallingConv::C) return false;
    if (Callee->getName() != "realloc") return false;
    if (CB->arg_size() != 2) return false;
    if (!CB->getType()->isPointerTy()) return false;
    if (!CB->getArgOperand(0)->getType()->isPointerTy()) return false;
    Type *SizeTy = DL.getIntPtrType(CB->getContext());
    if (CB->getArgOperand(1)->getType() != SizeTy) return false;
    return true;
}

/**
 * @brief Checks if a call instruction represents a compatible standard free call.
 */
static bool isCompatibleFreeCall(const CallBase *CB) {
    Function *Callee = CB->getCalledFunction();
    if (!Callee || Callee->isIntrinsic()) return false;
    if (Callee->getCallingConv() != CallingConv::C || CB->getCallingConv() != CallingConv::C) return false;
    if (Callee->getName() != "free") return false;
    if (CB->arg_size() != 1) return false;
    if (!CB->getType()->isVoidTy()) return false;
    if (!CB->getArgOperand(0)->getType()->isPointerTy()) return false;
    return true;
}

/**
 * @brief Checks if a memory access instruction has already been instrumented.
 *
 * Prevents duplicate instrumentation by checking for pass metadata or an immediately
 * preceding call to mg_check_access with matching arguments.
 */
static bool isAlreadyInstrumented(const Instruction *I, const Value *Ptr, uint64_t Size) {
    if (I->getMetadata("memoryguard.checked")) {
        return true;
    }
    if (const Instruction *Prev = I->getPrevNode()) {
        if (auto *CB = dyn_cast<CallBase>(Prev)) {
            if (const Function *Callee = CB->getCalledFunction()) {
                if (Callee->getName() == "mg_check_access" && CB->arg_size() == 2) {
                    if (CB->getArgOperand(0) == Ptr) {
                        if (auto *CI = dyn_cast<ConstantInt>(CB->getArgOperand(1))) {
                            if (CI->getZExtValue() == Size) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

/**
 * @brief MemoryGuard Function Pass for analyzing LLVM memory operations and heap allocations.
 *
 * Inspects each function for:
 *  1. Heap allocation API calls (malloc, calloc, realloc).
 *  2. Memory access instructions (LoadInst and StoreInst) with pointer-origin classification.
 *  3. Instruments HEAP_CANDIDATE memory accesses with runtime boundary validation checks.
 */
class MemoryGuardPass : public PassInfoMixin<MemoryGuardPass> {
public:
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
        if (F.isDeclaration() || isMemoryGuardRuntimeFunction(F)) {
            return PreservedAnalyses::all();
        }

        errs() << "MemoryGuard: analyzing function " << F.getName() << "\n";

        const DataLayout &DL = F.getParent()->getDataLayout();
        DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
        Module *M = F.getParent();

        InterproceduralContext ICtx;
        ICtx.CurrentDT = &DT;

        enum class RewriteKind { MALLOC, CALLOC, REALLOC, FREE };
        struct CallRewriteSite {
            CallBase *CB;
            RewriteKind Kind;
        };
        SmallVector<CallRewriteSite, 8> CallsToRewrite;

        struct InstrumentationSite {
            Instruction *Inst;
            Value *Ptr;
            uint64_t AccessSize;
        };
        SmallVector<InstrumentationSite, 16> SitesToInstrument;

        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *CB = dyn_cast<CallBase>(&I)) {
                    analyzeAllocation(F, CB);
                    if (isCompatibleMallocCall(CB, DL)) {
                        CallsToRewrite.push_back({CB, RewriteKind::MALLOC});
                    } else if (isCompatibleCallocCall(CB, DL)) {
                        CallsToRewrite.push_back({CB, RewriteKind::CALLOC});
                    } else if (isCompatibleReallocCall(CB, DL)) {
                        CallsToRewrite.push_back({CB, RewriteKind::REALLOC});
                    } else if (isCompatibleFreeCall(CB)) {
                        CallsToRewrite.push_back({CB, RewriteKind::FREE});
                    }
                } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    Value *Ptr = LI->getPointerOperand();
                    Type *AccessedType = LI->getType();
                    TypeSize StoreSize = DL.getTypeStoreSize(AccessedType);
                    PointerOrigin Origin = classifyPointerOrigin(Ptr, &DT, ICtx);

                    errs() << "  [READ]  function=" << F.getName()
                           << " type=";
                    AccessedType->print(errs());
                    errs() << " ptr=";
                    Ptr->printAsOperand(errs(), false);
                    errs() << " size=";
                    if (StoreSize.isScalable()) {
                        errs() << "scalable (min " << StoreSize.getKnownMinValue() << " bytes)";
                    } else {
                        errs() << StoreSize.getFixedValue() << " bytes";
                    }
                    errs() << " origin=" << originToString(Origin) << "\n";

                    // Instrument only HEAP_CANDIDATE load accesses with fixed sizes and standard address space
                    if (Origin == PointerOrigin::HEAP_CANDIDATE && !StoreSize.isScalable()) {
                        if (auto *PT = dyn_cast<PointerType>(Ptr->getType())) {
                            if (PT->getAddressSpace() == 0) {
                                uint64_t FixedSize = StoreSize.getFixedValue();
                                if (!isAlreadyInstrumented(LI, Ptr, FixedSize)) {
                                    SitesToInstrument.push_back({LI, Ptr, FixedSize});
                                }
                            }
                        }
                    }
                } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    Value *Ptr = SI->getPointerOperand();
                    Type *StoredType = SI->getValueOperand()->getType();
                    TypeSize StoreSize = DL.getTypeStoreSize(StoredType);
                    PointerOrigin Origin = classifyPointerOrigin(Ptr, &DT, ICtx);

                    errs() << "  [WRITE] function=" << F.getName()
                           << " type=";
                    StoredType->print(errs());
                    errs() << " ptr=";
                    Ptr->printAsOperand(errs(), false);
                    errs() << " size=";
                    if (StoreSize.isScalable()) {
                        errs() << "scalable (min " << StoreSize.getKnownMinValue() << " bytes)";
                    } else {
                        errs() << StoreSize.getFixedValue() << " bytes";
                    }
                    errs() << " origin=" << originToString(Origin) << "\n";

                    // Instrument only HEAP_CANDIDATE store accesses with fixed sizes and standard address space
                    if (Origin == PointerOrigin::HEAP_CANDIDATE && !StoreSize.isScalable()) {
                        if (auto *PT = dyn_cast<PointerType>(Ptr->getType())) {
                            if (PT->getAddressSpace() == 0) {
                                uint64_t FixedSize = StoreSize.getFixedValue();
                                if (!isAlreadyInstrumented(SI, Ptr, FixedSize)) {
                                    SitesToInstrument.push_back({SI, Ptr, FixedSize});
                                }
                            }
                        }
                    }
                }
            }
        }

        bool Modified = false;

        // Perform allocator call-site rewrites
        for (const auto &Site : CallsToRewrite) {
            switch (Site.Kind) {
            case RewriteKind::MALLOC: {
                FunctionCallee InterceptMalloc = getOrInsertInterceptMalloc(*M, DL);
                Site.CB->setCalledFunction(InterceptMalloc);
                Site.CB->removeFnAttr(Attribute::AllocSize);
                Site.CB->removeFnAttr(Attribute::Builtin);
                Modified = true;
                break;
            }
            case RewriteKind::CALLOC: {
                FunctionCallee InterceptCalloc = getOrInsertInterceptCalloc(*M, DL);
                Site.CB->setCalledFunction(InterceptCalloc);
                Site.CB->removeFnAttr(Attribute::AllocSize);
                Site.CB->removeFnAttr(Attribute::Builtin);
                Modified = true;
                break;
            }
            case RewriteKind::REALLOC: {
                FunctionCallee InterceptRealloc = getOrInsertInterceptRealloc(*M, DL);
                Site.CB->setCalledFunction(InterceptRealloc);
                Site.CB->removeFnAttr(Attribute::AllocSize);
                Site.CB->removeFnAttr(Attribute::Builtin);
                Modified = true;
                break;
            }
            case RewriteKind::FREE: {
                FunctionCallee InterceptFree = getOrInsertInterceptFree(*M);
                Site.CB->setCalledFunction(InterceptFree);
                Site.CB->removeFnAttr(Attribute::Builtin);
                Modified = true;
                break;
            }
            }
        }

        // Perform heap check instrumentation
        if (!SitesToInstrument.empty()) {
            FunctionCallee CheckFn = getOrInsertCheckFunction(*M, DL);
            LLVMContext &Ctx = F.getContext();
            Type *SizeTy = DL.getIntPtrType(Ctx);

            for (const auto &Site : SitesToInstrument) {
                IRBuilder<> Builder(Site.Inst);
                Value *SizeVal = ConstantInt::get(SizeTy, Site.AccessSize);
                CallInst *CheckCall = Builder.CreateCall(CheckFn, {Site.Ptr, SizeVal});
                CheckCall->setCallingConv(CallingConv::C);
                Site.Inst->setMetadata("memoryguard.checked", MDNode::get(Ctx, {}));
            }
            Modified = true;
        }

        if (!Modified) {
            return PreservedAnalyses::all();
        }

        PreservedAnalyses PA;
        PA.preserve<DominatorTreeAnalysis>();
        return PA;
    }

    // Ensure the pass runs on all functions, including unoptimized (-O0) code
    static bool isRequired() { return true; }

private:
    void analyzeAllocation(const Function &F, const CallBase *CB) {
        Function *Callee = CB->getCalledFunction();
        if (!Callee || Callee->isIntrinsic()) {
            return;
        }

        // Standard heap allocation APIs return a pointer
        if (!CB->getType()->isPointerTy()) {
            return;
        }

        // Require both the callsite and the called function to use the C calling convention
        if (Callee->getCallingConv() != CallingConv::C || CB->getCallingConv() != CallingConv::C) {
            return;
        }

        StringRef CalleeName = Callee->getName();

        if (CalleeName == "malloc" || CalleeName == "mg_malloc" || CalleeName == "mg_intercept_malloc") {
            if (CB->arg_size() != 1) {
                return;
            }
            Value *SizeArg = CB->getArgOperand(0);

            errs() << "  [ALLOC] function=" << F.getName()
                   << " api=" << CalleeName << " ptr=";
            CB->printAsOperand(errs(), false);
            errs() << " size=";
            if (auto *CI = dyn_cast<ConstantInt>(SizeArg)) {
                errs() << CI->getZExtValue() << " bytes\n";
            } else {
                errs() << "expr(";
                SizeArg->printAsOperand(errs(), false);
                errs() << ")\n";
            }
        } else if (CalleeName == "calloc" || CalleeName == "mg_intercept_calloc") {
            if (CB->arg_size() != 2) {
                return;
            }
            Value *CountArg = CB->getArgOperand(0);
            Value *SizeArg = CB->getArgOperand(1);

            errs() << "  [ALLOC] function=" << F.getName()
                   << " api=" << CalleeName << " ptr=";
            CB->printAsOperand(errs(), false);
            errs() << " size=";

            auto *CI0 = dyn_cast<ConstantInt>(CountArg);
            auto *CI1 = dyn_cast<ConstantInt>(SizeArg);
            if (CI0 && CI1) {
                unsigned MaxWidth = std::max({CI0->getBitWidth(), CI1->getBitWidth(), 64u});
                APInt CountVal = CI0->getValue().zextOrTrunc(MaxWidth);
                APInt SizeVal = CI1->getValue().zextOrTrunc(MaxWidth);
                bool Overflow = false;
                APInt Total = CountVal.umul_ov(SizeVal, Overflow);
                if (!Overflow) {
                    errs() << Total.getZExtValue() << " bytes ("
                           << CI0->getZExtValue() << " x " << CI1->getZExtValue() << ")\n";
                } else {
                    errs() << "overflow (" << CI0->getZExtValue() << " x " << CI1->getZExtValue() << ")\n";
                }
            } else {
                errs() << "expr(";
                CountArg->printAsOperand(errs(), false);
                errs() << " * ";
                SizeArg->printAsOperand(errs(), false);
                errs() << ")\n";
            }
        } else if (CalleeName == "realloc" || CalleeName == "mg_intercept_realloc") {
            if (CB->arg_size() != 2) {
                return;
            }
            Value *OldPtr = CB->getArgOperand(0);
            Value *SizeArg = CB->getArgOperand(1);

            errs() << "  [ALLOC] function=" << F.getName()
                   << " api=" << CalleeName << " ptr=";
            CB->printAsOperand(errs(), false);
            errs() << " old_ptr=";
            OldPtr->printAsOperand(errs(), false);
            errs() << " size=";
            if (auto *CI = dyn_cast<ConstantInt>(SizeArg)) {
                errs() << CI->getZExtValue() << " bytes\n";
            } else {
                errs() << "expr(";
                SizeArg->printAsOperand(errs(), false);
                errs() << ")\n";
            }
        }
    }
};

} // namespace

/**
 * @brief Public entry point for the LLVM pass plugin.
 *
 * Registers the 'memoryguard' pass into the LLVM PassBuilder pipeline parser callback.
 */
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION,
        "MemoryGuard",
        LLVM_VERSION_STRING,
        [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "memoryguard") {
                        FPM.addPass(MemoryGuardPass());
                        return true;
                    }
                    return false;
                });
        }
    };
}
