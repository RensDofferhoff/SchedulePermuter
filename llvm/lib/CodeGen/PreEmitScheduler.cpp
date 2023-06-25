#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PriorityQueue.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachinePassRegistry.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/RegisterClassInfo.h"
#include "llvm/CodeGen/RegisterPressure.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/CodeGen/ScheduleDAGMutation.h"
#include "llvm/CodeGen/ScheduleDFS.h"
#include "llvm/CodeGen/ScheduleHazardRecognizer.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSchedule.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/LaneBitmask.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/MachineValueType.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <string>

#include "llvm/CodeGen/SchedulePermuterHooks.h"
#include "ExpDAGPrinter.h"

using namespace llvm;

#define DEBUG_TYPE "machine-scheduler"
#define LLVM_ENABLE_DUMP "dump"



//===----------------------------------------------------------------------===//
// External Machine Instruction Scheduler
//===----------------------------------------------------------------------===//

SUnit* (*pickNodeExtern) (bool &IsTopNode);
void (*releaseTopNodeExtern) (SUnit *SU);
void (*initExtern) ();
bool bypassPreEmitScheduler;
bool printSchedulerMBBs = false;
bool printScheduledMBBs = false;
bool printScheduleDAGs = true;
std::string hookOutputPath;


class ExternalScheduler : public MachineSchedStrategy {

public:
  ExternalScheduler(){}

  void initialize(ScheduleDAGMI*) override {
    initExtern();
  }

  /// Implement MachineSchedStrategy interface.
  /// -----------------------------------------

  SUnit *pickNode(bool &IsTopNode) override {
    return pickNodeExtern(IsTopNode);
  }

  void schedNode(SUnit *SU, bool IsTopNode) override {}

  void releaseTopNode(SUnit *SU) override {
      releaseTopNodeExtern(SU);
  }

  void releaseBottomNode(SUnit *SU) override {}
};



//===----------------------------------------------------------------------===//
// Machine Instruction Scheduling Pass and Registry
//===----------------------------------------------------------------------===//


namespace {

/// Base class for a machine scheduler class that can run at any point.
class MachineSchedulerBase : public MachineSchedContext,
                             public MachineFunctionPass {
public:
  MachineSchedulerBase(char &ID): MachineFunctionPass(ID) {}

  void print(raw_ostream &O, const Module* = nullptr) const override;

protected:
  void scheduleRegions(ScheduleDAGInstrs &Scheduler, bool FixKillFlags);
};

/// PostMachineScheduler runs after shortly before code emission.
class PreEmitScheduler : public MachineSchedulerBase {
public:
  PreEmitScheduler();

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  bool runOnMachineFunction(MachineFunction&) override;

  static char ID; // Class identification, replacement for typeinfo

protected:
  ScheduleDAGInstrs *createPreEmitMachineScheduler();

private:
  
};

} // end anonymous namespace


char PreEmitScheduler::ID = 0;

char &llvm::PreEmitSchedulerID = PreEmitScheduler::ID;

INITIALIZE_PASS_BEGIN(PreEmitScheduler, "preemitmisched",
                      "PreEmit Machine Instruction Scheduler", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfo)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(PreEmitScheduler, "preemitmisched",
                    "PreEmit Machine Instruction Scheduler", false, false)

PreEmitScheduler::PreEmitScheduler() : MachineSchedulerBase(ID) {
  initializePreEmitSchedulerPass(*PassRegistry::getPassRegistry());
}

FunctionPass *llvm::createPreEmitSchedulerPass() { return new PreEmitScheduler(); }

void PreEmitScheduler::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<MachineDominatorTree>();
  AU.addRequired<MachineLoopInfo>();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addRequired<TargetPassConfig>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

/// Decrement this iterator until reaching the top or a non-debug instr.
static MachineBasicBlock::const_iterator
priorNonDebug(MachineBasicBlock::const_iterator I,
              MachineBasicBlock::const_iterator Beg) {
  assert(I != Beg && "reached the top of the region, cannot decrement");
  while (--I != Beg) {
    if (!I->isDebugOrPseudoInstr())
      break;
  }
  return I;
}

/// Non-const version.
static MachineBasicBlock::iterator
priorNonDebug(MachineBasicBlock::iterator I,
              MachineBasicBlock::const_iterator Beg) {
  return priorNonDebug(MachineBasicBlock::const_iterator(I), Beg).getNonConstIterator();
}

/// If this iterator is a debug value, increment until reaching the End or a
/// non-debug instruction.
static MachineBasicBlock::const_iterator
nextIfDebug(MachineBasicBlock::const_iterator I,
            MachineBasicBlock::const_iterator End) {
  for(; I != End; ++I) {
    if (!I->isDebugOrPseudoInstr())
      break;
  }
  return I;
}

/// Non-const version.
static MachineBasicBlock::iterator
nextIfDebug(MachineBasicBlock::iterator I,
            MachineBasicBlock::const_iterator End) {
  return nextIfDebug(MachineBasicBlock::const_iterator(I), End)
      .getNonConstIterator();
}


/// Instantiate a ScheduleDAGInstrs for PostRA scheduling that will be owned by
/// the caller. We don't have a command line option to override the postRA
/// scheduler. The Target must configure it.
ScheduleDAGInstrs *PreEmitScheduler::createPreEmitMachineScheduler() {
  // Default to GenericScheduler.
  return new ScheduleDAGMI(this, std::make_unique<ExternalScheduler>(),/*RemoveKillFlags=*/true);
}

/// Top-level MachineScheduler pass driver.
///
/// Visit blocks in function order. Divide each block into scheduling regions
/// and visit them bottom-up. Visiting regions bottom-up is not required, but is
/// consistent with the DAG builder, which traverses the interior of the
/// scheduling regions bottom-up.
///
/// This design avoids exposing scheduling boundaries to the DAG builder,
/// simplifying the DAG builder's support for "special" target instructions.
/// At the same time the design allows target schedulers to operate across
/// scheduling boundaries, for example to bundle the boundary instructions
/// without reordering them. This creates complexity, because the target
/// scheduler must update the RegionBegin and RegionEnd positions cached by
/// ScheduleDAGInstrs whenever adding or removing instructions. A much simpler
/// design would be to split blocks at scheduling boundaries, but LLVM has a
/// general bias against block splitting purely for implementation simplicity.
bool PreEmitScheduler::runOnMachineFunction(MachineFunction &mf) {
  if (skipFunction(mf.getFunction()) || bypassPreEmitScheduler)
    return false;

  // Initialize the context of the pass.
  MF = &mf;
  MLI = &getAnalysis<MachineLoopInfo>();
  PassConfig = &getAnalysis<TargetPassConfig>();
  AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();

  if (VerifyScheduling)
    MF->verify(this, "Before scheduling.");


  if(mf.getName() == SchedOnlyFunc && printSchedulerMBBs) {
    for(auto& MBB : mf) {
      outs() << "\nMBB num: " << MBB.getNumber() << "\n";
      MBB.dump();
    }
  }

  std::unique_ptr<ScheduleDAGInstrs> Scheduler(createPreEmitMachineScheduler());
  scheduleRegions(*Scheduler, true);

  if(mf.getName() == SchedOnlyFunc) {
    printScheduleDAGs = false;
  }


  if (VerifyScheduling)
    MF->verify(this, "After scheduling.");
  return true;
}

/// Return true of the given instruction should not be included in a scheduling
/// region.
///
/// MachineScheduler does not currently support scheduling across calls. To
/// handle calls, the DAG builder needs to be modified to create register
/// anti/output dependencies on the registers clobbered by the call's regmask
/// operand. In PreRA scheduling, the stack pointer adjustment already prevents
/// scheduling across calls. In PostRA scheduling, we need the isCall to enforce
/// the boundary, but there would be no benefit to postRA scheduling across
/// calls this late anyway.
static bool isSchedBoundary(MachineBasicBlock::iterator MI,
                            MachineBasicBlock *MBB,
                            MachineFunction *MF,
                            const TargetInstrInfo *TII) {
  return MI->isCall() || TII->isSchedulingBoundary(*MI, MBB, *MF);
}

/// A region of an MBB for scheduling.
namespace {
struct SchedRegion {
  /// RegionBegin is the first instruction in the scheduling region, and
  /// RegionEnd is either MBB->end() or the scheduling boundary after the
  /// last instruction in the scheduling region. These iterators cannot refer
  /// to instructions outside of the identified scheduling region because
  /// those may be reordered before scheduling this region.
  MachineBasicBlock::iterator RegionBegin;
  MachineBasicBlock::iterator RegionEnd;
  unsigned NumRegionInstrs;

  SchedRegion(MachineBasicBlock::iterator B, MachineBasicBlock::iterator E,
              unsigned N) :
    RegionBegin(B), RegionEnd(E), NumRegionInstrs(N) {}
};
} // end anonymous namespace

using MBBRegionsVector = SmallVector<SchedRegion, 16>;

static void
getSchedRegions(MachineBasicBlock *MBB,
                MBBRegionsVector &Regions,
                bool RegionsTopDown) {
  MachineFunction *MF = MBB->getParent();
  const TargetInstrInfo *TII = MF->getSubtarget().getInstrInfo();

  MachineBasicBlock::iterator I = nullptr;
  for(MachineBasicBlock::iterator RegionEnd = MBB->end();
      RegionEnd != MBB->begin(); RegionEnd = I) {

    // Avoid decrementing RegionEnd for blocks with no terminator.
    if (RegionEnd != MBB->end() ||
        isSchedBoundary(&*std::prev(RegionEnd), &*MBB, MF, TII)) {
      --RegionEnd;
    }

    // The next region starts above the previous region. Look backward in the
    // instruction stream until we find the nearest boundary.
    unsigned NumRegionInstrs = 0;
    I = RegionEnd;
    for (;I != MBB->begin(); --I) {
      MachineInstr &MI = *std::prev(I);
      if (isSchedBoundary(&MI, &*MBB, MF, TII))
        break;
      if (!MI.isDebugOrPseudoInstr()) {
        // MBB::size() uses instr_iterator to count. Here we need a bundle to
        // count as a single instruction.
        ++NumRegionInstrs;
      }
    }

    // It's possible we found a scheduling region that only has debug
    // instructions. Don't bother scheduling these.
    if (NumRegionInstrs != 0)
      Regions.push_back(SchedRegion(I, RegionEnd, NumRegionInstrs));
  }

  if (RegionsTopDown)
    std::reverse(Regions.begin(), Regions.end());
}

/// Main driver for both MachineScheduler and PostMachineScheduler.
void MachineSchedulerBase::scheduleRegions(ScheduleDAGInstrs &Scheduler,
                                           bool FixKillFlags) {
   

  for (MachineFunction::iterator MBB = MF->begin(), MBBEnd = MF->end();
       MBB != MBBEnd; ++MBB) {
    
    if (SchedOnlyFunc != MF->getName())
      continue;

    if (SchedOnlyBlock.find(MBB->getNumber()) == SchedOnlyBlock.end())
      continue;

    Scheduler.startBlock(&*MBB);

    int regionCounter = 0;
    MBBRegionsVector MBBRegions;
    getSchedRegions(&*MBB, MBBRegions, Scheduler.doMBBSchedRegionsTopDown());
    for (MBBRegionsVector::iterator R = MBBRegions.begin();
         R != MBBRegions.end(); ++R) {
      MachineBasicBlock::iterator I = R->RegionBegin;
      MachineBasicBlock::iterator RegionEnd = R->RegionEnd;
      unsigned NumRegionInstrs = R->NumRegionInstrs;

      // Notify the scheduler of the region, even if we may skip scheduling
      // it. Perhaps it still needs to be bundled.
      Scheduler.enterRegion(&*MBB, I, RegionEnd, NumRegionInstrs);

      // Skip empty scheduling regions (0 or 1 schedulable instructions).
      if (I == RegionEnd || I == std::prev(RegionEnd)) {
        // Close the current region. Bundle the terminator if needed.
        // This invalidates 'RegionEnd' and 'I'.
        Scheduler.exitRegion();
        continue;
      }
      regionCounter++;
      Scheduler.schedule();
      
      if(printScheduleDAGs) {
        std::string name = MBB->getFullName() + "_" + std::to_string(MBB->getNumber()) + "_" + std::to_string(regionCounter);
        WriteExpGraph(&Scheduler, name);
      }

      
      // Close the current region.
      Scheduler.exitRegion();
    }
    Scheduler.finishBlock();
    if (FixKillFlags)
      Scheduler.fixupKills(*MBB);

    if(printScheduledMBBs) MBB->dump();
  }
  Scheduler.finalizeSchedule();
}

void MachineSchedulerBase::print(raw_ostream &O, const Module* m) const {
  // unimplemented
}

void WriteExpGraph(ScheduleDAGInstrs* DAG, std::string name) {
  llvm::WriteGraph(static_cast<ScheduleDAG*>(DAG), name, false, name, hookOutputPath + name);
}