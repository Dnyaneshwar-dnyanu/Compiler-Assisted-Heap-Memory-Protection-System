#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
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
#include "llvm/Support/AtomicOrdering.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TypeSize.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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

[[maybe_unused]] static StringRef originToString(PointerOrigin Origin) {
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

static StringRef atomicOrderingToString(AtomicOrdering Ordering) {
    switch (Ordering) {
    case AtomicOrdering::NotAtomic:
        return "not_atomic";
    case AtomicOrdering::Unordered:
        return "unordered";
    case AtomicOrdering::Monotonic:
        return "monotonic";
    case AtomicOrdering::Acquire:
        return "acquire";
    case AtomicOrdering::Release:
        return "release";
    case AtomicOrdering::AcquireRelease:
        return "acq_rel";
    case AtomicOrdering::SequentiallyConsistent:
        return "seq_cst";
    }
    return "unknown";
}

static StringRef atomicRMWOpToString(AtomicRMWInst::BinOp Op) {
    switch (Op) {
    case AtomicRMWInst::Xchg: return "xchg";
    case AtomicRMWInst::Add:  return "add";
    case AtomicRMWInst::Sub:  return "sub";
    case AtomicRMWInst::And:  return "and";
    case AtomicRMWInst::Nand: return "nand";
    case AtomicRMWInst::Or:   return "or";
    case AtomicRMWInst::Xor:  return "xor";
    case AtomicRMWInst::Max:  return "max";
    case AtomicRMWInst::Min:  return "min";
    case AtomicRMWInst::UMax: return "umax";
    case AtomicRMWInst::UMin: return "umin";
    case AtomicRMWInst::FAdd: return "fadd";
    case AtomicRMWInst::FSub: return "fsub";
    case AtomicRMWInst::FMax: return "fmax";
    case AtomicRMWInst::FMin: return "fmin";
    case AtomicRMWInst::UIncWrap: return "uinc_wrap";
    case AtomicRMWInst::UDecWrap: return "udec_wrap";
    default: return "rmw_op";
    }
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
    if (!Callee || Callee->isIntrinsic() || Callee->isVarArg()) {
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

static PointerOrigin traceExtractedValueOrigin(const Value *Agg,
                                               ArrayRef<unsigned> Indices,
                                               const DominatorTree *DT,
                                               SmallPtrSetImpl<const Value *> &Visited,
                                               unsigned Depth,
                                               InterproceduralContext &ICtx);

static PointerOrigin traceArgumentAggregateOrigin(const Argument *Arg,
                                                  ArrayRef<unsigned> Indices,
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
 * @brief Interprocedural aggregate origin analysis for function parameters.
 *
 * Follows aggregate values passed into function parameters at direct call sites
 * to trace extracted pointer fields.
 */
static PointerOrigin traceArgumentAggregateOrigin(const Argument *Arg,
                                                  ArrayRef<unsigned> Indices,
                                                  SmallPtrSetImpl<const Value *> &Visited,
                                                  unsigned Depth,
                                                  InterproceduralContext &ICtx) {
    if (!Arg || Indices.empty()) {
        return PointerOrigin::UNKNOWN;
    }

    const Function *F = Arg->getParent();
    if (!F || F->isDeclaration() || !F->hasLocalLinkage() || F->hasAddressTaken() || F->use_empty()) {
        return PointerOrigin::UNKNOWN;
    }

    unsigned ArgNo = Arg->getArgNo();
    PointerOrigin CommonOrigin = PointerOrigin::UNKNOWN;
    bool FirstCallSite = true;

    for (const Use &U : F->uses()) {
        const User *UserObj = U.getUser();
        auto *CB = dyn_cast<CallBase>(UserObj);
        if (!CB || !CB->isCallee(&U) || ArgNo >= CB->arg_size()) {
            return PointerOrigin::UNKNOWN;
        }

        Value *ActualArg = CB->getArgOperand(ArgNo);
        if (!ActualArg) {
            return PointerOrigin::UNKNOWN;
        }

        const Function *Caller = CB->getFunction();
        if (!Caller || Caller->isDeclaration() || isMemoryGuardRuntimeFunction(*Caller)) {
            return PointerOrigin::UNKNOWN;
        }

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

        PointerOrigin ActualOrigin = traceExtractedValueOrigin(ActualArg, Indices, CallerDT, Visited, Depth + 1, ICtx);
        if (ActualOrigin == PointerOrigin::UNKNOWN) {
            return PointerOrigin::UNKNOWN;
        }

        if (FirstCallSite) {
            CommonOrigin = ActualOrigin;
            FirstCallSite = false;
        } else if (CommonOrigin != ActualOrigin) {
            return PointerOrigin::UNKNOWN;
        }
    }

    return CommonOrigin;
}

/**
 * @brief Traces the origin of a pointer extracted at @p Indices from an aggregate @p Agg.
 *
 * Handles InsertValueInst, Constants, Selects, PHIs, Freezes, Loads from alloca,
 * and Arguments with nested struct and array indices.
 */
static PointerOrigin traceExtractedValueOrigin(const Value *Agg,
                                               ArrayRef<unsigned> Indices,
                                               const DominatorTree *DT,
                                               SmallPtrSetImpl<const Value *> &Visited,
                                               unsigned Depth,
                                               InterproceduralContext &ICtx) {
    if (!Agg || Indices.empty()) {
        return PointerOrigin::UNKNOWN;
    }

    if (Depth > MaxTraversalDepth) {
        return PointerOrigin::UNKNOWN;
    }

    PathVisitedScope Scope(Visited, Agg);
    if (Scope.isCycle()) {
        return PointerOrigin::UNKNOWN;
    }

    // 1. InsertValueInst
    if (auto *IVI = dyn_cast<InsertValueInst>(Agg)) {
        ArrayRef<unsigned> InsertIndices = IVI->getIndices();

        // Exact index match: the extracted value is the value inserted here
        if (Indices == InsertIndices) {
            return tracePointerOrigin(IVI->getInsertedValueOperand(), DT, Visited, Depth + 1, ICtx);
        }

        // Nested aggregate insertion: InsertIndices is a prefix of Indices
        // e.g. inserted a sub-struct at {0}, and extracting {0, 1}
        if (InsertIndices.size() < Indices.size() &&
            Indices.take_front(InsertIndices.size()) == InsertIndices) {
            ArrayRef<unsigned> RemainingIndices = Indices.drop_front(InsertIndices.size());
            return traceExtractedValueOrigin(IVI->getInsertedValueOperand(),
                                             RemainingIndices,
                                             DT, Visited, Depth + 1, ICtx);
        }

        // If Indices is a prefix of InsertIndices, a sub-element of the extracted pointer is modified
        if (Indices.size() < InsertIndices.size() &&
            InsertIndices.take_front(Indices.size()) == Indices) {
            return PointerOrigin::UNKNOWN;
        }

        // Disjoint indices: the insertion does not affect the extracted field. Follow the base aggregate.
        return traceExtractedValueOrigin(IVI->getAggregateOperand(),
                                         Indices,
                                         DT, Visited, Depth + 1, ICtx);
    }

    // 2. Constants
    if (auto *C = dyn_cast<Constant>(Agg)) {
        if (isa<UndefValue>(C)) {
            return PointerOrigin::UNKNOWN;
        }
        if (isa<ConstantAggregateZero>(C)) {
            return PointerOrigin::NON_HEAP;
        }

        const Constant *Curr = C;
        for (unsigned Idx : Indices) {
            if (!Curr) break;
            Curr = Curr->getAggregateElement(Idx);
        }

        if (Curr && Curr->getType()->isPointerTy()) {
            return tracePointerOrigin(Curr, DT, Visited, Depth + 1, ICtx);
        }
        return PointerOrigin::UNKNOWN;
    }

    // 3. Select instructions on aggregates
    if (auto *SI = dyn_cast<SelectInst>(Agg)) {
        PointerOrigin TrueOrigin = traceExtractedValueOrigin(SI->getTrueValue(), Indices, DT, Visited, Depth + 1, ICtx);
        PointerOrigin FalseOrigin = traceExtractedValueOrigin(SI->getFalseValue(), Indices, DT, Visited, Depth + 1, ICtx);
        if (TrueOrigin == FalseOrigin && TrueOrigin != PointerOrigin::UNKNOWN) {
            return TrueOrigin;
        }
        return PointerOrigin::UNKNOWN;
    }

    // 4. PHI nodes on aggregates
    if (auto *PN = dyn_cast<PHINode>(Agg)) {
        if (PN->getNumIncomingValues() == 0) {
            return PointerOrigin::UNKNOWN;
        }
        PointerOrigin CommonOrigin = PointerOrigin::UNKNOWN;
        bool First = true;
        for (const Value *Incoming : PN->incoming_values()) {
            PointerOrigin IncOrigin = traceExtractedValueOrigin(Incoming, Indices, DT, Visited, Depth + 1, ICtx);
            if (IncOrigin == PointerOrigin::UNKNOWN) {
                return PointerOrigin::UNKNOWN;
            }
            if (First) {
                CommonOrigin = IncOrigin;
                First = false;
            } else if (CommonOrigin != IncOrigin) {
                return PointerOrigin::UNKNOWN;
            }
        }
        return CommonOrigin;
    }

    // 5. Freeze instructions on aggregates
    if (auto *FI = dyn_cast<FreezeInst>(Agg)) {
        return traceExtractedValueOrigin(FI->getOperand(0), Indices, DT, Visited, Depth + 1, ICtx);
    }

    // 6. Load instruction from an alloca storing an aggregate
    if (auto *LI = dyn_cast<LoadInst>(Agg)) {
        if (LI->isAtomic()) {
            return PointerOrigin::UNKNOWN;
        }
        if (auto *AI = dyn_cast<AllocaInst>(LI->getPointerOperand())) {
            SmallVector<const StoreInst *, 4> Stores;
            for (const User *U : AI->users()) {
                if (auto *SI = dyn_cast<StoreInst>(U)) {
                    if (SI->getValueOperand() == AI) {
                        return PointerOrigin::UNKNOWN;
                    }
                    if (SI->getPointerOperand() == AI) {
                        if (SI->isAtomic()) {
                            return PointerOrigin::UNKNOWN;
                        }
                        Stores.push_back(SI);
                    }
                } else if (isa<LoadInst>(U) || isa<IntrinsicInst>(U)) {
                    continue;
                } else {
                    return PointerOrigin::UNKNOWN;
                }
            }

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
                return traceExtractedValueOrigin(LocalDominatingStore->getValueOperand(),
                                                 Indices, DT, Visited, Depth + 1, ICtx);
            }

            if (Stores.size() == 1 && DT) {
                const StoreInst *SingleStore = Stores[0];
                if (DT->dominates(SingleStore, LI)) {
                    return traceExtractedValueOrigin(SingleStore->getValueOperand(),
                                                     Indices, DT, Visited, Depth + 1, ICtx);
                }
            }
        }
        return PointerOrigin::UNKNOWN;
    }

    // 7. Function argument passing an aggregate
    if (auto *Arg = dyn_cast<Argument>(Agg)) {
        return traceArgumentAggregateOrigin(Arg, Indices, Visited, Depth, ICtx);
    }

    return PointerOrigin::UNKNOWN;
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
                if (SI->isAtomic()) {
                    return PointerOrigin::UNKNOWN;
                }
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

    // 8. Pointer casts: follow safe pointer-preserving casts only (BitCast).
    // Return UNKNOWN for PtrToInt, IntToPtr, AddrSpaceCast, or other unsafe conversions.
    if (auto *Cast = dyn_cast<CastInst>(V)) {
        if (isa<BitCastInst>(Cast)) {
            return tracePointerOrigin(Cast->getOperand(0), DT, Visited, Depth + 1, ICtx);
        }
        return PointerOrigin::UNKNOWN;
    }

    // 9. Freeze instructions
    if (auto *FI = dyn_cast<FreezeInst>(V)) {
        return tracePointerOrigin(FI->getOperand(0), DT, Visited, Depth + 1, ICtx);
    }

    // 10. Constant expressions (GEP, safe pointer bitcast, or select)
    if (auto *CE = dyn_cast<ConstantExpr>(V)) {
        if (CE->getOpcode() == Instruction::GetElementPtr ||
            CE->getOpcode() == Instruction::BitCast) {
            return tracePointerOrigin(CE->getOperand(0), DT, Visited, Depth + 1, ICtx);
        }
        if (CE->getOpcode() == Instruction::Select) {
            PointerOrigin TrueOrigin = tracePointerOrigin(CE->getOperand(1), DT, Visited, Depth + 1, ICtx);
            PointerOrigin FalseOrigin = tracePointerOrigin(CE->getOperand(2), DT, Visited, Depth + 1, ICtx);
            if (TrueOrigin == FalseOrigin && TrueOrigin != PointerOrigin::UNKNOWN) {
                return TrueOrigin;
            }
            return PointerOrigin::UNKNOWN;
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
        if (LI->isAtomic()) {
            return PointerOrigin::UNKNOWN;
        }
        const Value *LoadSource = LI->getPointerOperand();
        if (auto *AI = dyn_cast<AllocaInst>(LoadSource)) {
            return traceAllocaOrigin(AI, LI, DT, Visited, Depth + 1, ICtx);
        }
        return PointerOrigin::UNKNOWN;
    }

    // 14. ExtractValue instructions: trace extracted pointer through aggregate operations
    if (auto *EVI = dyn_cast<ExtractValueInst>(V)) {
        if (EVI->getType()->isPointerTy()) {
            return traceExtractedValueOrigin(EVI->getAggregateOperand(),
                                             EVI->getIndices(),
                                             DT, Visited, Depth + 1, ICtx);
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
    if (!Callee || Callee->isIntrinsic() || Callee->isVarArg()) return false;
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
    if (!Callee || Callee->isIntrinsic() || Callee->isVarArg()) return false;
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
    if (!Callee || Callee->isIntrinsic() || Callee->isVarArg()) return false;
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
    if (!Callee || Callee->isIntrinsic() || Callee->isVarArg()) return false;
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
 * @brief Analysis statistics counters for MemoryGuard diagnostics.
 */
struct AnalysisStats {
    uint64_t TotalAccessesExamined = 0;        // Total load and store instructions examined
    uint64_t HeapAccessesIdentified = 0;       // Heap-origin accesses identified
    uint64_t ChecksInserted = 0;               // Runtime checks inserted
    uint64_t VolatileAccessesInstrumented = 0; // Volatile accesses instrumented
    uint64_t NonHeapAccessesSkipped = 0;       // Non-heap accesses skipped
    uint64_t UnknownAccessesSkipped = 0;       // Unknown-origin accesses skipped
    uint64_t AtomicOperationsSkipped = 0;      // Atomic operations skipped
    uint64_t UnsupportedAddrSpaceSkipped = 0;  // Unsupported address-space accesses skipped
    uint64_t ScalableVectorSkipped = 0;        // Scalable-vector accesses skipped
    uint64_t AllocatorsRewritten = 0;          // Allocator calls detected and successfully rewritten
    uint64_t IncompatibleAllocatorsSkipped = 0;// Incompatible or unsupported allocator calls skipped

    uint64_t totalUnsupportedSkipped() const {
        return AtomicOperationsSkipped + ScalableVectorSkipped +
               UnsupportedAddrSpaceSkipped + IncompatibleAllocatorsSkipped;
    }

    void add(const AnalysisStats &Other) {
        TotalAccessesExamined += Other.TotalAccessesExamined;
        HeapAccessesIdentified += Other.HeapAccessesIdentified;
        ChecksInserted += Other.ChecksInserted;
        VolatileAccessesInstrumented += Other.VolatileAccessesInstrumented;
        NonHeapAccessesSkipped += Other.NonHeapAccessesSkipped;
        UnknownAccessesSkipped += Other.UnknownAccessesSkipped;
        AtomicOperationsSkipped += Other.AtomicOperationsSkipped;
        UnsupportedAddrSpaceSkipped += Other.UnsupportedAddrSpaceSkipped;
        ScalableVectorSkipped += Other.ScalableVectorSkipped;
        AllocatorsRewritten += Other.AllocatorsRewritten;
        IncompatibleAllocatorsSkipped += Other.IncompatibleAllocatorsSkipped;
    }
};

/**
 * @brief Checks if a call base represents an allocator function candidate.
 */
static bool isAllocatorCandidate(const CallBase *CB) {
    if (const Function *Callee = CB->getCalledFunction()) {
        StringRef Name = Callee->getName();
        if (Name == "malloc" || Name == "calloc" || Name == "realloc" || Name == "free" ||
            Name.starts_with("variadic_malloc") || Name.starts_with("variadic_free") ||
            Name.starts_with("variadic_") ||
            (Name.contains("malloc") && !Name.starts_with("mg_")) ||
            (Name.contains("calloc") && !Name.starts_with("mg_")) ||
            (Name.contains("realloc") && !Name.starts_with("mg_")) ||
            (Name.contains("free") && !Name.starts_with("mg_") && !Name.contains("freeze"))) {
            return true;
        }
    } else if (CB->isIndirectCall()) {
        Value *CalledOp = CB->getCalledOperand();
        if (CalledOp && (CalledOp->getName().contains("alloc") ||
                         CalledOp->getName().contains("malloc") ||
                         CalledOp->getName().contains("free"))) {
            return true;
        }
    }
    return false;
}

static void printFunctionSummary(StringRef FuncName, const AnalysisStats &Stats) {
    errs() << "[MemoryGuard][Function] @" << FuncName << ":\n";
    errs() << "  Accesses examined:               " << Stats.TotalAccessesExamined << "\n";
    errs() << "  Checks inserted:                 " << Stats.ChecksInserted << "\n";
    errs() << "  Heap accesses identified:        " << Stats.HeapAccessesIdentified << "\n";
    errs() << "  Non-heap accesses skipped:       " << Stats.NonHeapAccessesSkipped << "\n";
    errs() << "  Unknown-origin accesses skipped: " << Stats.UnknownAccessesSkipped << "\n";
    errs() << "  Unsupported operations skipped:  " << Stats.totalUnsupportedSkipped() << "\n";
}

static void printModuleSummary(const AnalysisStats &Stats) {
    errs() << "[MemoryGuard][Summary] Module Analysis Summary:\n";
    errs() << "  Total load and store instructions examined:         " << Stats.TotalAccessesExamined << "\n";
    errs() << "  Heap-origin accesses identified:                    " << Stats.HeapAccessesIdentified << "\n";
    errs() << "  Runtime checks inserted:                            " << Stats.ChecksInserted << "\n";
    errs() << "  Volatile accesses instrumented:                     " << Stats.VolatileAccessesInstrumented << "\n";
    errs() << "  Non-heap accesses skipped:                          " << Stats.NonHeapAccessesSkipped << "\n";
    errs() << "  Unknown-origin accesses skipped:                    " << Stats.UnknownAccessesSkipped << "\n";
    errs() << "  Atomic operations skipped:                          " << Stats.AtomicOperationsSkipped << "\n";
    errs() << "  Unsupported address-space accesses skipped:         " << Stats.UnsupportedAddrSpaceSkipped << "\n";
    errs() << "  Scalable-vector accesses skipped:                   " << Stats.ScalableVectorSkipped << "\n";
    errs() << "  Allocator calls detected and successfully rewritten: " << Stats.AllocatorsRewritten << "\n";
    errs() << "  Incompatible or unsupported allocator calls skipped: " << Stats.IncompatibleAllocatorsSkipped << "\n";
}

/**
 * @brief Structured representation of a statically provable heap memory violation.
 */
struct StaticViolationDiagnostic {
    std::string ErrorType; // "STATIC_HEAP_OUT_OF_BOUNDS_WRITE" or "STATIC_HEAP_OUT_OF_BOUNDS_READ"
    std::string Severity;  // "error"
    std::string File;
    unsigned Line = 0;
    unsigned Column = 0;
    std::string AccessType; // "write" or "read"
    uint64_t AllocationSize = 0;
    uint64_t AccessSize = 0;
    int64_t Offset = 0;
    int64_t BytesOutOfBounds = 0;
    std::string Statement;
    std::string Message;
};

/**
 * @brief Result of tracing pointer provenance for static bounds evaluation.
 */
struct StaticHeapProvenanceResult {
    bool IsHeap = false;
    uint64_t AllocSize = 0;
    int64_t ByteOffset = 0;
    bool HasConstantOffset = true;
    const Value *AllocOrigin = nullptr;
};

/**
 * @brief Extracts statically known heap allocation size from standard allocator calls.
 */
static bool getHeapAllocationSize(const CallBase *CB, uint64_t &OutAllocSize) {
    if (!CB) return false;
    Function *Callee = CB->getCalledFunction();
    if (!Callee) return false;
    StringRef Name = Callee->getName();

    if ((Name == "malloc" || Name == "mg_malloc" || Name == "mg_intercept_malloc") &&
        CB->arg_size() >= 1) {
        if (auto *CI = dyn_cast<ConstantInt>(CB->getArgOperand(0))) {
            OutAllocSize = CI->getZExtValue();
            return true;
        }
    } else if ((Name == "calloc" || Name == "mg_intercept_calloc") &&
               CB->arg_size() >= 2) {
        auto *CI0 = dyn_cast<ConstantInt>(CB->getArgOperand(0));
        auto *CI1 = dyn_cast<ConstantInt>(CB->getArgOperand(1));
        if (CI0 && CI1) {
            bool Overflow = false;
            APInt Total = CI0->getValue().umul_ov(CI1->getValue(), Overflow);
            if (!Overflow) {
                OutAllocSize = Total.getZExtValue();
                return true;
            }
        }
    } else if ((Name == "realloc" || Name == "mg_intercept_realloc") &&
               CB->arg_size() >= 2) {
        if (auto *CI = dyn_cast<ConstantInt>(CB->getArgOperand(1))) {
            OutAllocSize = CI->getZExtValue();
            return true;
        }
    } else if ((Name == "_Znwm" || Name == "_Znam") &&
               CB->arg_size() >= 1) {
        if (auto *CI = dyn_cast<ConstantInt>(CB->getArgOperand(0))) {
            OutAllocSize = CI->getZExtValue();
            return true;
        }
    }
    return false;
}

/**
 * @brief Recursively traces pointer value to find heap allocation base and constant byte offset.
 */
static StaticHeapProvenanceResult analyzeStaticHeapProvenance(
    const Value *V,
    const DataLayout &DL,
    const DominatorTree *DT,
    SmallPtrSetImpl<const Value *> &Visited,
    unsigned Depth) {

    if (!V || Depth > 16 || Visited.count(V)) {
        return {false, 0, 0, false, nullptr};
    }
    Visited.insert(V);

    // 1. GEP Instruction: accumulate constant offset
    if (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
        APInt GEPConstantOffset(DL.getIndexTypeSizeInBits(GEP->getType()), 0, true);
        if (!GEP->accumulateConstantOffset(DL, GEPConstantOffset)) {
            StaticHeapProvenanceResult Sub = analyzeStaticHeapProvenance(
                GEP->getPointerOperand(), DL, DT, Visited, Depth + 1);
            Sub.HasConstantOffset = false;
            return Sub;
        }
        int64_t ThisOffset = GEPConstantOffset.getSExtValue();
        StaticHeapProvenanceResult Sub = analyzeStaticHeapProvenance(
            GEP->getPointerOperand(), DL, DT, Visited, Depth + 1);
        if (Sub.HasConstantOffset) {
            Sub.ByteOffset += ThisOffset;
        }
        return Sub;
    }

    // 2. Casts (BitCast)
    if (auto *Cast = dyn_cast<CastInst>(V)) {
        if (isa<BitCastInst>(Cast)) {
            return analyzeStaticHeapProvenance(Cast->getOperand(0), DL, DT, Visited, Depth + 1);
        }
        return {false, 0, 0, false, nullptr};
    }

    // 3. Freeze instruction
    if (auto *FI = dyn_cast<FreezeInst>(V)) {
        return analyzeStaticHeapProvenance(FI->getOperand(0), DL, DT, Visited, Depth + 1);
    }

    // 4. Load from alloca (unoptimized -O0 variable tracking)
    if (auto *LI = dyn_cast<LoadInst>(V)) {
        const Value *LoadPtr = LI->getPointerOperand();
        if (auto *AI = dyn_cast<AllocaInst>(LoadPtr)) {
            const StoreInst *LocalDominatingStore = nullptr;
            for (const Instruction *Prev = LI->getPrevNode(); Prev != nullptr; Prev = Prev->getPrevNode()) {
                if (auto *SI = dyn_cast<StoreInst>(Prev)) {
                    if (SI->getPointerOperand() == AI) {
                        LocalDominatingStore = SI;
                        break;
                    }
                }
            }
            if (LocalDominatingStore) {
                return analyzeStaticHeapProvenance(LocalDominatingStore->getValueOperand(), DL, DT, Visited, Depth + 1);
            }
            SmallVector<const StoreInst *, 4> Stores;
            for (const User *U : AI->users()) {
                if (auto *SI = dyn_cast<StoreInst>(U)) {
                    if (SI->getPointerOperand() == AI) {
                        Stores.push_back(SI);
                    }
                }
            }
            if (Stores.size() == 1 && DT && DT->dominates(Stores[0], LI)) {
                return analyzeStaticHeapProvenance(Stores[0]->getValueOperand(), DL, DT, Visited, Depth + 1);
            }
        }
        return {false, 0, 0, false, nullptr};
    }

    // 5. Direct heap allocation call
    if (auto *CB = dyn_cast<CallBase>(V)) {
        uint64_t AllocSize = 0;
        if (getHeapAllocationSize(CB, AllocSize)) {
            return {true, AllocSize, 0, true, CB};
        }
        if (isHeapAllocCall(CB)) {
            return {true, 0, 0, false, CB};
        }
    }

    return {false, 0, 0, false, nullptr};
}

static std::string escapeJsonString(const std::string &Str) {
    std::string Out;
    for (char C : Str) {
        if (C == '"') Out += "\\\"";
        else if (C == '\\') Out += "\\\\";
        else if (C == '\b') Out += "\\b";
        else if (C == '\f') Out += "\\f";
        else if (C == '\n') Out += "\\n";
        else if (C == '\r') Out += "\\r";
        else if (C == '\t') Out += "\\t";
        else Out += C;
    }
    return Out;
}

static void writeStaticDiagnosticsJson(const std::vector<StaticViolationDiagnostic> &Diags, const std::string &Path) {
    std::error_code EC;
    raw_fd_ostream Out(Path, EC, sys::fs::OF_Text);
    if (EC) {
        errs() << "[MemoryGuard] Failed to open " << Path << " for writing: " << EC.message() << "\n";
        return;
    }

    Out << "[\n";
    for (size_t i = 0; i < Diags.size(); ++i) {
        const auto &D = Diags[i];
        Out << "  {\n";
        Out << "    \"errorType\": \"" << escapeJsonString(D.ErrorType) << "\",\n";
        Out << "    \"severity\": \"" << escapeJsonString(D.Severity) << "\",\n";
        Out << "    \"file\": \"" << escapeJsonString(D.File) << "\",\n";
        Out << "    \"line\": " << D.Line << ",\n";
        Out << "    \"column\": " << D.Column << ",\n";
        Out << "    \"accessType\": \"" << escapeJsonString(D.AccessType) << "\",\n";
        Out << "    \"allocationSize\": " << D.AllocationSize << ",\n";
        Out << "    \"accessSize\": " << D.AccessSize << ",\n";
        Out << "    \"offset\": " << D.Offset << ",\n";
        Out << "    \"bytesOutOfBounds\": " << D.BytesOutOfBounds << ",\n";
        Out << "    \"statement\": \"" << escapeJsonString(D.Statement) << "\",\n";
        Out << "    \"message\": \"" << escapeJsonString(D.Message) << "\"\n";
        Out << "  }" << (i + 1 < Diags.size() ? "," : "") << "\n";
    }
    Out << "]\n";
}

static void checkStaticHeapAccess(
    Instruction *Inst,
    Value *Ptr,
    uint64_t AccessSize,
    bool IsWrite,
    const DataLayout &DL,
    const DominatorTree *DT,
    std::vector<StaticViolationDiagnostic> &StaticViolations) {

    if (!Inst || !Ptr || AccessSize == 0) {
        return;
    }

    SmallPtrSet<const Value *, 8> Visited;
    StaticHeapProvenanceResult Res = analyzeStaticHeapProvenance(Ptr, DL, DT, Visited, 0);

    if (Res.IsHeap && Res.HasConstantOffset && Res.AllocSize > 0) {
        bool IsViolation = false;
        int64_t BytesOOB = 0;

        if (Res.ByteOffset < 0) {
            IsViolation = true;
            BytesOOB = -Res.ByteOffset;
        } else if (Res.ByteOffset + (int64_t)AccessSize > (int64_t)Res.AllocSize) {
            IsViolation = true;
            BytesOOB = (Res.ByteOffset + (int64_t)AccessSize) - (int64_t)Res.AllocSize;
        }

        if (IsViolation) {
            StaticViolationDiagnostic Diag;
            Diag.ErrorType = IsWrite ? "STATIC_HEAP_OUT_OF_BOUNDS_WRITE" : "STATIC_HEAP_OUT_OF_BOUNDS_READ";
            Diag.Severity = "error";
            Diag.AccessType = IsWrite ? "write" : "read";
            Diag.AllocationSize = Res.AllocSize;
            Diag.AccessSize = AccessSize;
            Diag.Offset = Res.ByteOffset;
            Diag.BytesOutOfBounds = BytesOOB;
            Diag.File = "source.c";
            Diag.Line = 0;
            Diag.Column = 0;

            const DebugLoc &DLoc = Inst->getDebugLoc();
            if (DLoc) {
                Diag.Line = DLoc.getLine();
                Diag.Column = DLoc.getCol();
                if (!DLoc->getFilename().empty()) {
                    Diag.File = DLoc->getFilename().str();
                }
            }

            // Extract statement from source file if accessible
            if (Diag.Line > 0) {
                std::ifstream SrcFile(Diag.File);
                if (!SrcFile.is_open()) {
                    SrcFile.open("/src/" + Diag.File);
                }
                if (SrcFile.is_open()) {
                    std::string CurLine;
                    unsigned CurLineNum = 1;
                    while (std::getline(SrcFile, CurLine)) {
                        if (CurLineNum == Diag.Line) {
                            size_t First = CurLine.find_first_not_of(" \t\r\n");
                            size_t Last = CurLine.find_last_not_of(" \t\r\n");
                            if (First != std::string::npos && Last != std::string::npos) {
                                Diag.Statement = CurLine.substr(First, Last - First + 1);
                            } else {
                                Diag.Statement = CurLine;
                            }
                            break;
                        }
                        CurLineNum++;
                    }
                }
            }

            if (Res.ByteOffset < 0) {
                Diag.Message = "Definite heap out-of-bounds " + Diag.AccessType + ": access of " +
                               std::to_string(AccessSize) + " bytes at negative offset " +
                               std::to_string(Res.ByteOffset) + " precedes allocation boundary [0, " +
                               std::to_string(Res.AllocSize) + ").";
            } else {
                Diag.Message = "Definite heap out-of-bounds " + Diag.AccessType + ": access of " +
                               std::to_string(AccessSize) + " bytes at offset " +
                               std::to_string(Res.ByteOffset) + " exceeds allocation size of " +
                               std::to_string(Res.AllocSize) + " bytes (by " +
                               std::to_string(BytesOOB) + " bytes).";
            }

            // Avoid duplicate reports for the same instruction/line
            bool Duplicate = false;
            for (const auto &Existing : StaticViolations) {
                if (Existing.Line == Diag.Line && Existing.Column == Diag.Column &&
                    Existing.Offset == Diag.Offset && Existing.ErrorType == Diag.ErrorType) {
                    Duplicate = true;
                    break;
                }
            }
            if (!Duplicate) {
                StaticViolations.push_back(Diag);
            }
        }
    }
}

/**
 * @brief MemoryGuard Module Pass for analyzing LLVM memory operations and heap allocations.
 *
 * Inspects each function in the module for:
 *  1. Heap allocation API calls (malloc, calloc, realloc, free) and rewrites them to interceptors.
 *  2. Memory access instructions (LoadInst and StoreInst) with pointer-origin classification.
 *  3. Instruments HEAP_CANDIDATE memory accesses with runtime boundary validation checks.
 *  4. Collects and reports per-function and module-level analysis summaries.
 */
class MemoryGuardPass : public PassInfoMixin<MemoryGuardPass> {
public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
        FunctionAnalysisManager &FAM =
            MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
        const DataLayout &DL = M.getDataLayout();

        AnalysisStats ModuleStats;
        InterproceduralContext ICtx;
        bool ModuleModified = false;
        std::vector<StaticViolationDiagnostic> ModuleStaticViolations;

        for (Function &F : M) {
            if (F.isDeclaration() || isMemoryGuardRuntimeFunction(F)) {
                continue;
            }

            errs() << "[MemoryGuard] Analyzing function @" << F.getName() << "\n";

            DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
            ICtx.CurrentDT = &DT;

            AnalysisStats FuncStats;
            bool FuncModified = runOnFunction(F, M, DL, DT, ICtx, FuncStats, ModuleStaticViolations);
            if (FuncModified) {
                ModuleModified = true;
            }

            printFunctionSummary(F.getName(), FuncStats);
            ModuleStats.add(FuncStats);
        }

        printModuleSummary(ModuleStats);

        if (!ModuleStaticViolations.empty()) {
            std::string OutPath;
            if (const char *EnvPath = getenv("MG_STATIC_DIAGNOSTICS_PATH")) {
                OutPath = EnvPath;
            } else if (sys::fs::is_directory("/out")) {
                OutPath = "/out/static_diagnostics.json";
            } else {
                OutPath = "static_diagnostics.json";
            }

            SmallString<128> OutPathSS(OutPath);
            StringRef ParentDir = sys::path::parent_path(OutPathSS);
            if (!ParentDir.empty() && !sys::fs::is_directory(ParentDir)) {
                OutPath = "static_diagnostics.json";
            }

            writeStaticDiagnosticsJson(ModuleStaticViolations, OutPath);

            errs() << "\n=================================================================\n";
            errs() << "== MEMORYGUARD COMPILE-TIME VIOLATION DETECTED ==\n";
            for (const auto &V : ModuleStaticViolations) {
                errs() << "File           : " << V.File << ":" << V.Line << ":" << V.Column << "\n";
                errs() << "Violation Type : " << V.ErrorType << "\n";
                errs() << "Access Type    : " << V.AccessType << " (" << V.AccessSize << " bytes)\n";
                errs() << "Offset         : " << V.Offset << " bytes\n";
                errs() << "Allocation Size: " << V.AllocationSize << " bytes\n";
                if (!V.Statement.empty()) {
                    errs() << "Statement      : " << V.Statement << "\n";
                }
                errs() << "Explanation    : " << V.Message << "\n";
            }
            errs() << "=================================================================\n\n";
        }

        if (!ModuleModified) {
            return PreservedAnalyses::all();
        }

        PreservedAnalyses PA;
        PA.preserve<DominatorTreeAnalysis>();
        return PA;
    }

    // Ensure the pass runs on all modules/functions, including unoptimized (-O0) code
    static bool isRequired() { return true; }

private:
    enum class RewriteKind { MALLOC, CALLOC, REALLOC, FREE };

    struct CallRewriteSite {
        CallBase *CB;
        RewriteKind Kind;
    };

    struct InstrumentationSite {
        Instruction *Inst;
        Value *Ptr;
        uint64_t AccessSize;
    };

    bool runOnFunction(Function &F, Module &M, const DataLayout &DL,
                       const DominatorTree &DT, InterproceduralContext &ICtx,
                       AnalysisStats &Stats,
                       std::vector<StaticViolationDiagnostic> &StaticViolations) {
        SmallVector<CallRewriteSite, 8> CallsToRewrite;
        SmallVector<InstrumentationSite, 16> SitesToInstrument;

        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *CB = dyn_cast<CallBase>(&I)) {
                    analyzeAllocation(F, CB);
                    if (isCompatibleMallocCall(CB, DL)) {
                        CallsToRewrite.push_back({CB, RewriteKind::MALLOC});
                        Stats.AllocatorsRewritten++;
                        errs() << "[MemoryGuard]   [ALLOC REWRITE] function=@" << F.getName()
                               << " malloc -> mg_intercept_malloc\n";
                    } else if (isCompatibleCallocCall(CB, DL)) {
                        CallsToRewrite.push_back({CB, RewriteKind::CALLOC});
                        Stats.AllocatorsRewritten++;
                        errs() << "[MemoryGuard]   [ALLOC REWRITE] function=@" << F.getName()
                               << " calloc -> mg_intercept_calloc\n";
                    } else if (isCompatibleReallocCall(CB, DL)) {
                        CallsToRewrite.push_back({CB, RewriteKind::REALLOC});
                        Stats.AllocatorsRewritten++;
                        errs() << "[MemoryGuard]   [ALLOC REWRITE] function=@" << F.getName()
                               << " realloc -> mg_intercept_realloc\n";
                    } else if (isCompatibleFreeCall(CB)) {
                        CallsToRewrite.push_back({CB, RewriteKind::FREE});
                        Stats.AllocatorsRewritten++;
                        errs() << "[MemoryGuard]   [ALLOC REWRITE] function=@" << F.getName()
                               << " free -> mg_intercept_free\n";
                    } else if (isAllocatorCandidate(CB)) {
                        Stats.IncompatibleAllocatorsSkipped++;
                        errs() << "[MemoryGuard]   [ALLOC SKIP] function=@" << F.getName()
                               << " (skipped: incompatible or unsupported allocator call)\n";
                    }
                } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    Stats.TotalAccessesExamined++;
                    Value *Ptr = LI->getPointerOperand();
                    Type *AccessedType = LI->getType();
                    TypeSize StoreSize = DL.getTypeStoreSize(AccessedType);

                    if (!StoreSize.isScalable() && StoreSize.getFixedValue() > 0) {
                        checkStaticHeapAccess(LI, Ptr, StoreSize.getFixedValue(), /*IsWrite=*/false, DL, &DT, StaticViolations);
                    }

                    if (LI->isAtomic()) {
                        Stats.AtomicOperationsSkipped++;
                        errs() << "[MemoryGuard]   [ATOMIC READ] function=@" << F.getName()
                               << " type=";
                        AccessedType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " ordering=" << atomicOrderingToString(LI->getOrdering())
                               << " (skipped: atomic operation)\n";
                        continue;
                    }

                    if (StoreSize.isScalable()) {
                        Stats.ScalableVectorSkipped++;
                        errs() << "[MemoryGuard]   [READ] function=@" << F.getName()
                               << " type=";
                        AccessedType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " (skipped: scalable vector memory access unsupported)\n";
                        continue;
                    }

                    auto *PT = dyn_cast<PointerType>(Ptr->getType());
                    if (PT && PT->getAddressSpace() != 0) {
                        Stats.UnsupportedAddrSpaceSkipped++;
                        errs() << "[MemoryGuard]   [READ] function=@" << F.getName()
                               << " type=";
                        AccessedType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " (skipped: non-default address space " << PT->getAddressSpace() << " unsupported)\n";
                        continue;
                    }

                    PointerOrigin Origin = classifyPointerOrigin(Ptr, &DT, ICtx);
                    if (Origin == PointerOrigin::NON_HEAP) {
                        Stats.NonHeapAccessesSkipped++;
                        errs() << (LI->isVolatile() ? "[MemoryGuard]   [READ] (volatile)" : "[MemoryGuard]   [READ]")
                               << " function=@" << F.getName()
                               << " type=";
                        AccessedType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " size=" << StoreSize.getFixedValue() << " bytes"
                               << " origin=NON_HEAP\n";
                    } else if (Origin == PointerOrigin::UNKNOWN) {
                        Stats.UnknownAccessesSkipped++;
                        errs() << (LI->isVolatile() ? "[MemoryGuard]   [READ] (volatile)" : "[MemoryGuard]   [READ]")
                               << " function=@" << F.getName()
                               << " type=";
                        AccessedType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " size=" << StoreSize.getFixedValue() << " bytes"
                               << " origin=UNKNOWN\n";
                    } else if (Origin == PointerOrigin::HEAP_CANDIDATE) {
                        Stats.HeapAccessesIdentified++;
                        errs() << (LI->isVolatile() ? "[MemoryGuard]   [READ] (volatile)" : "[MemoryGuard]   [READ]")
                               << " function=@" << F.getName()
                               << " type=";
                        AccessedType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " size=" << StoreSize.getFixedValue() << " bytes"
                               << " origin=HEAP_CANDIDATE\n";

                        uint64_t FixedSize = StoreSize.getFixedValue();
                        if (isAlreadyInstrumented(LI, Ptr, FixedSize)) {
                            errs() << "    (already instrumented, skipping duplicate check)\n";
                        } else {
                            Stats.ChecksInserted++;
                            if (LI->isVolatile()) {
                                Stats.VolatileAccessesInstrumented++;
                            }
                            SitesToInstrument.push_back({LI, Ptr, FixedSize});
                        }
                    }
                } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    Stats.TotalAccessesExamined++;
                    Value *Ptr = SI->getPointerOperand();
                    Type *StoredType = SI->getValueOperand()->getType();
                    TypeSize StoreSize = DL.getTypeStoreSize(StoredType);

                    if (!StoreSize.isScalable() && StoreSize.getFixedValue() > 0) {
                        checkStaticHeapAccess(SI, Ptr, StoreSize.getFixedValue(), /*IsWrite=*/true, DL, &DT, StaticViolations);
                    }

                    if (SI->isAtomic()) {
                        Stats.AtomicOperationsSkipped++;
                        errs() << "[MemoryGuard]   [ATOMIC WRITE] function=@" << F.getName()
                               << " type=";
                        StoredType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " ordering=" << atomicOrderingToString(SI->getOrdering())
                               << " (skipped: atomic operation)\n";
                        continue;
                    }

                    if (StoreSize.isScalable()) {
                        Stats.ScalableVectorSkipped++;
                        errs() << "[MemoryGuard]   [WRITE] function=@" << F.getName()
                               << " type=";
                        StoredType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " (skipped: scalable vector memory access unsupported)\n";
                        continue;
                    }

                    auto *PT = dyn_cast<PointerType>(Ptr->getType());
                    if (PT && PT->getAddressSpace() != 0) {
                        Stats.UnsupportedAddrSpaceSkipped++;
                        errs() << "[MemoryGuard]   [WRITE] function=@" << F.getName()
                               << " type=";
                        StoredType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " (skipped: non-default address space " << PT->getAddressSpace() << " unsupported)\n";
                        continue;
                    }

                    PointerOrigin Origin = classifyPointerOrigin(Ptr, &DT, ICtx);
                    if (Origin == PointerOrigin::NON_HEAP) {
                        Stats.NonHeapAccessesSkipped++;
                        errs() << (SI->isVolatile() ? "[MemoryGuard]   [WRITE] (volatile)" : "[MemoryGuard]   [WRITE]")
                               << " function=@" << F.getName()
                               << " type=";
                        StoredType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " size=" << StoreSize.getFixedValue() << " bytes"
                               << " origin=NON_HEAP\n";
                    } else if (Origin == PointerOrigin::UNKNOWN) {
                        Stats.UnknownAccessesSkipped++;
                        errs() << (SI->isVolatile() ? "[MemoryGuard]   [WRITE] (volatile)" : "[MemoryGuard]   [WRITE]")
                               << " function=@" << F.getName()
                               << " type=";
                        StoredType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " size=" << StoreSize.getFixedValue() << " bytes"
                               << " origin=UNKNOWN\n";
                    } else if (Origin == PointerOrigin::HEAP_CANDIDATE) {
                        Stats.HeapAccessesIdentified++;
                        errs() << (SI->isVolatile() ? "[MemoryGuard]   [WRITE] (volatile)" : "[MemoryGuard]   [WRITE]")
                               << " function=@" << F.getName()
                               << " type=";
                        StoredType->print(errs());
                        errs() << " ptr=";
                        Ptr->printAsOperand(errs(), false);
                        errs() << " size=" << StoreSize.getFixedValue() << " bytes"
                               << " origin=HEAP_CANDIDATE\n";

                        uint64_t FixedSize = StoreSize.getFixedValue();
                        if (isAlreadyInstrumented(SI, Ptr, FixedSize)) {
                            errs() << "    (already instrumented, skipping duplicate check)\n";
                        } else {
                            Stats.ChecksInserted++;
                            if (SI->isVolatile()) {
                                Stats.VolatileAccessesInstrumented++;
                            }
                            SitesToInstrument.push_back({SI, Ptr, FixedSize});
                        }
                    }
                } else if (auto *RMW = dyn_cast<AtomicRMWInst>(&I)) {
                    Stats.AtomicOperationsSkipped++;
                    Value *Ptr = RMW->getPointerOperand();
                    Type *ValType = RMW->getValOperand()->getType();
                    errs() << "[MemoryGuard]   [ATOMIC RMW] function=@" << F.getName()
                           << " op=" << atomicRMWOpToString(RMW->getOperation())
                           << " type=";
                    ValType->print(errs());
                    errs() << " ptr=";
                    Ptr->printAsOperand(errs(), false);
                    errs() << " ordering=" << atomicOrderingToString(RMW->getOrdering())
                           << " (skipped: atomic operation)\n";
                } else if (auto *CXI = dyn_cast<AtomicCmpXchgInst>(&I)) {
                    Stats.AtomicOperationsSkipped++;
                    Value *Ptr = CXI->getPointerOperand();
                    Type *ValType = CXI->getCompareOperand()->getType();
                    errs() << "[MemoryGuard]   [ATOMIC CMPXCHG] function=@" << F.getName()
                           << " type=";
                    ValType->print(errs());
                    errs() << " ptr=";
                    Ptr->printAsOperand(errs(), false);
                    errs() << " success_ordering=" << atomicOrderingToString(CXI->getSuccessOrdering())
                           << " failure_ordering=" << atomicOrderingToString(CXI->getFailureOrdering())
                           << " (skipped: atomic operation)\n";
                } else if (auto *FI = dyn_cast<FenceInst>(&I)) {
                    Stats.AtomicOperationsSkipped++;
                    errs() << "[MemoryGuard]   [ATOMIC FENCE] function=@" << F.getName()
                           << " ordering=" << atomicOrderingToString(FI->getOrdering())
                           << " (skipped: no pointer operand)\n";
                }
            }
        }

        bool Modified = false;

        // Perform allocator call-site rewrites
        for (const auto &Site : CallsToRewrite) {
            switch (Site.Kind) {
            case RewriteKind::MALLOC: {
                FunctionCallee InterceptMalloc = getOrInsertInterceptMalloc(M, DL);
                Site.CB->setCalledFunction(InterceptMalloc);
                Site.CB->removeFnAttr(Attribute::AllocSize);
                Site.CB->removeFnAttr(Attribute::Builtin);
                Modified = true;
                break;
            }
            case RewriteKind::CALLOC: {
                FunctionCallee InterceptCalloc = getOrInsertInterceptCalloc(M, DL);
                Site.CB->setCalledFunction(InterceptCalloc);
                Site.CB->removeFnAttr(Attribute::AllocSize);
                Site.CB->removeFnAttr(Attribute::Builtin);
                Modified = true;
                break;
            }
            case RewriteKind::REALLOC: {
                FunctionCallee InterceptRealloc = getOrInsertInterceptRealloc(M, DL);
                Site.CB->setCalledFunction(InterceptRealloc);
                Site.CB->removeFnAttr(Attribute::AllocSize);
                Site.CB->removeFnAttr(Attribute::Builtin);
                Modified = true;
                break;
            }
            case RewriteKind::FREE: {
                FunctionCallee InterceptFree = getOrInsertInterceptFree(M);
                Site.CB->setCalledFunction(InterceptFree);
                Site.CB->removeFnAttr(Attribute::Builtin);
                Modified = true;
                break;
            }
            }
        }

        // Perform heap check instrumentation
        if (!SitesToInstrument.empty()) {
            FunctionCallee CheckFn = getOrInsertCheckFunction(M, DL);
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

        return Modified;
    }

private:
    void analyzeAllocation(const Function &F, const CallBase *CB) {
        Function *Callee = CB->getCalledFunction();
        if (!Callee || Callee->isIntrinsic() || Callee->isVarArg()) {
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

            errs() << "[MemoryGuard]   [ALLOC] function=@" << F.getName()
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

            errs() << "[MemoryGuard]   [ALLOC] function=@" << F.getName()
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

            errs() << "[MemoryGuard]   [ALLOC] function=@" << F.getName()
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
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    if (Name == "memoryguard") {
                        MPM.addPass(MemoryGuardPass());
                        return true;
                    }
                    return false;
                });
        }
    };
}
