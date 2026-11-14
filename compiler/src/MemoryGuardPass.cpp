#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/TypeSize.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

/**
 * @brief MemoryGuard Function Pass for analyzing LLVM memory operations.
 *
 * Inspects each function for memory access instructions (LoadInst and StoreInst),
 * identifies the access kind (READ/WRITE), accessed type, target pointer operand,
 * and memory access size in bytes.
 */
class MemoryGuardPass : public PassInfoMixin<MemoryGuardPass> {
public:
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
        errs() << "MemoryGuard: analyzing function " << F.getName() << "\n";

        const DataLayout &DL = F.getParent()->getDataLayout();

        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *LI = dyn_cast<LoadInst>(&I)) {
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
