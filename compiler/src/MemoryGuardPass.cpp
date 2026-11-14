#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
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
 * @brief MemoryGuard Function Pass for analyzing LLVM memory operations and heap allocations.
 *
 * Inspects each function for:
 *  1. Heap allocation API calls (malloc, calloc, realloc).
 *  2. Memory access instructions (LoadInst and StoreInst).
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

                    errs() << "  [READ]  function=" << F.getName()
                           << " type=";
                    AccessedType->print(errs());
                    errs() << " ptr=";
                    Ptr->printAsOperand(errs(), false);
                    errs() << " size=";
                    if (StoreSize.isScalable()) {
                        errs() << "scalable (min " << StoreSize.getKnownMinValue() << " bytes)\n";
                    } else {
                        errs() << StoreSize.getFixedValue() << " bytes\n";
                    }
                } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    Value *Ptr = SI->getPointerOperand();
                    Type *StoredType = SI->getValueOperand()->getType();
                    TypeSize StoreSize = DL.getTypeStoreSize(StoredType);

                    errs() << "  [WRITE] function=" << F.getName()
                           << " type=";
                    StoredType->print(errs());
                    errs() << " ptr=";
                    Ptr->printAsOperand(errs(), false);
                    errs() << " size=";
                    if (StoreSize.isScalable()) {
                        errs() << "scalable (min " << StoreSize.getKnownMinValue() << " bytes)\n";
                    } else {
                        errs() << StoreSize.getFixedValue() << " bytes\n";
                    }
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
