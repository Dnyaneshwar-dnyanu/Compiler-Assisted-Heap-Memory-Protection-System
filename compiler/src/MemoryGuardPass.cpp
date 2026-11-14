#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
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
    if (Callee->getCallingConv() != CallingConv::C && CB->getCallingConv() != CallingConv::C) {
        return false;
    }

    StringRef Name = Callee->getName();
    if (Name == "malloc" && CB->arg_size() == 1) {
        return true;
    }
    if (Name == "calloc" && CB->arg_size() == 2) {
        return true;
    }
    if (Name == "realloc" && CB->arg_size() == 2) {
        return true;
    }
    return false;
}

// Forward declaration
static PointerOrigin tracePointerOrigin(const Value *V,
                                        SmallPtrSetImpl<const Value *> &Visited,
                                        unsigned Depth);

static constexpr unsigned MaxTraversalDepth = 8;

/**
 * @brief Conservatively analyzes pointer values loaded from local memory (alloca).
 *
 * An alloca itself represents stack memory. However, if a pointer loaded from an
 * alloca is dereferenced, that loaded pointer may point to heap memory.
 * This helper traces the value(s) stored into the alloca if its address never escapes.
 */
static PointerOrigin traceAllocaOrigin(const AllocaInst *AI, const LoadInst *LI,
                                       SmallPtrSetImpl<const Value *> &Visited,
                                       unsigned Depth) {
    // Collect all stores to AI and check if AI's address escapes
    SmallVector<const StoreInst *, 4> Stores;
    for (const User *U : AI->users()) {
        if (auto *SI = dyn_cast<StoreInst>(U)) {
            // If AI is stored as a value (e.g. storing &AI into another pointer), it escapes
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
            // Any other use (call argument, GEP, etc.) means AI potentially escapes
            return PointerOrigin::UNKNOWN;
        }
    }

    if (Stores.empty()) {
        return PointerOrigin::UNKNOWN;
    }

    // Attempt to find the dominating store within the same basic block preceding LI
    const StoreInst *DominatingStore = nullptr;
    for (const Instruction *I = LI->getPrevNode(); I != nullptr; I = I->getPrevNode()) {
        if (auto *SI = dyn_cast<StoreInst>(I)) {
            if (SI->getPointerOperand() == AI) {
                DominatingStore = SI;
                break;
            }
        }
    }

    if (DominatingStore) {
        return tracePointerOrigin(DominatingStore->getValueOperand(), Visited, Depth + 1);
    }

    // If no dominating store in the same BB, check if all stores in the function agree
    PointerOrigin CommonOrigin = PointerOrigin::UNKNOWN;
    bool First = true;
    for (const StoreInst *SI : Stores) {
        PointerOrigin StoredOrigin = tracePointerOrigin(SI->getValueOperand(), Visited, Depth + 1);
        if (StoredOrigin == PointerOrigin::UNKNOWN) {
            return PointerOrigin::UNKNOWN;
        }
        if (First) {
            CommonOrigin = StoredOrigin;
            First = false;
        } else if (CommonOrigin != StoredOrigin) {
            return PointerOrigin::UNKNOWN;
        }
    }

    return CommonOrigin;
}

/**
 * @brief Recursive pointer-origin tracing with cycle detection and depth limiting.
 */
static PointerOrigin tracePointerOrigin(const Value *V,
                                        SmallPtrSetImpl<const Value *> &Visited,
                                        unsigned Depth) {
    if (!V) {
        return PointerOrigin::UNKNOWN;
    }

    // Depth limit to prevent infinite recursion and overhead
    if (Depth > MaxTraversalDepth) {
        return PointerOrigin::UNKNOWN;
    }

    // Cycle detection using visited set
    if (!Visited.insert(V).second) {
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

    // 6. Function arguments (unknown origin without interprocedural analysis)
    if (isa<Argument>(V)) {
        return PointerOrigin::UNKNOWN;
    }

    // 7. GEP instructions: follow to source pointer operand
    if (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
        return tracePointerOrigin(GEP->getPointerOperand(), Visited, Depth + 1);
    }

    // 8. Pointer casts (BitCast, AddrSpaceCast, IntToPtr, PtrToInt)
    if (auto *Cast = dyn_cast<CastInst>(V)) {
        return tracePointerOrigin(Cast->getOperand(0), Visited, Depth + 1);
    }

    // 9. Freeze instructions
    if (auto *FI = dyn_cast<FreezeInst>(V)) {
        return tracePointerOrigin(FI->getOperand(0), Visited, Depth + 1);
    }

    // 10. Constant expressions (GEP or Cast)
    if (auto *CE = dyn_cast<ConstantExpr>(V)) {
        if (CE->getOpcode() == Instruction::GetElementPtr || CE->isCast()) {
            return tracePointerOrigin(CE->getOperand(0), Visited, Depth + 1);
        }
    }

    // 11. Select instructions: evaluate both arms
    if (auto *SI = dyn_cast<SelectInst>(V)) {
        PointerOrigin TrueOrigin = tracePointerOrigin(SI->getTrueValue(), Visited, Depth + 1);
        PointerOrigin FalseOrigin = tracePointerOrigin(SI->getFalseValue(), Visited, Depth + 1);
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
            PointerOrigin IncomingOrigin = tracePointerOrigin(Incoming, Visited, Depth + 1);
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

    // 13. Load instructions: conservative local memory tracking
    if (auto *LI = dyn_cast<LoadInst>(V)) {
        const Value *LoadSource = LI->getPointerOperand();
        if (auto *AI = dyn_cast<AllocaInst>(LoadSource)) {
            return traceAllocaOrigin(AI, LI, Visited, Depth + 1);
        }
        return PointerOrigin::UNKNOWN;
    }

    return PointerOrigin::UNKNOWN;
}

/**
 * @brief Public helper to classify a pointer's origin.
 */
static PointerOrigin classifyPointerOrigin(const Value *V) {
    SmallPtrSet<const Value *, 16> Visited;
    return tracePointerOrigin(V, Visited, 0);
}

/**
 * @brief MemoryGuard Function Pass for analyzing LLVM memory operations and heap allocations.
 *
 * Inspects each function for:
 *  1. Heap allocation API calls (malloc, calloc, realloc).
 *  2. Memory access instructions (LoadInst and StoreInst) with pointer-origin classification.
 */
class MemoryGuardPass : public PassInfoMixin<MemoryGuardPass> {
public:
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
        if (F.isDeclaration()) {
            return PreservedAnalyses::all();
        }

        errs() << "MemoryGuard: analyzing function " << F.getName() << "\n";

        const DataLayout &DL = F.getParent()->getDataLayout();

        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *CB = dyn_cast<CallBase>(&I)) {
                    analyzeAllocation(F, CB);
                } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    Value *Ptr = LI->getPointerOperand();
                    Type *AccessedType = LI->getType();
                    TypeSize StoreSize = DL.getTypeStoreSize(AccessedType);
                    PointerOrigin Origin = classifyPointerOrigin(Ptr);

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
                } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    Value *Ptr = SI->getPointerOperand();
                    Type *StoredType = SI->getValueOperand()->getType();
                    TypeSize StoreSize = DL.getTypeStoreSize(StoredType);
                    PointerOrigin Origin = classifyPointerOrigin(Ptr);

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
                }
            }
        }

        return PreservedAnalyses::all();
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

        // Avoid false positives from unusual calling conventions
        if (Callee->getCallingConv() != CallingConv::C && CB->getCallingConv() != CallingConv::C) {
            return;
        }

        StringRef CalleeName = Callee->getName();

        if (CalleeName == "malloc") {
            if (CB->arg_size() != 1) {
                return;
            }
            Value *SizeArg = CB->getArgOperand(0);

            errs() << "  [ALLOC] function=" << F.getName()
                   << " api=malloc ptr=";
            CB->printAsOperand(errs(), false);
            errs() << " size=";
            if (auto *CI = dyn_cast<ConstantInt>(SizeArg)) {
                errs() << CI->getZExtValue() << " bytes\n";
            } else {
                errs() << "expr(";
                SizeArg->printAsOperand(errs(), false);
                errs() << ")\n";
            }
        } else if (CalleeName == "calloc") {
            if (CB->arg_size() != 2) {
                return;
            }
            Value *CountArg = CB->getArgOperand(0);
            Value *SizeArg = CB->getArgOperand(1);

            errs() << "  [ALLOC] function=" << F.getName()
                   << " api=calloc ptr=";
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
        } else if (CalleeName == "realloc") {
            if (CB->arg_size() != 2) {
                return;
            }
            Value *OldPtr = CB->getArgOperand(0);
            Value *SizeArg = CB->getArgOperand(1);

            errs() << "  [ALLOC] function=" << F.getName()
                   << " api=realloc ptr=";
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
