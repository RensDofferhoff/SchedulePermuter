//===-- RegAllocLrU.cpp - LrU Register Allocator ----------------------===//
//
// Based on Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the RALRU function pass, which provides a minimal
// implementation of the LRU register allocator.
//
//===----------------------------------------------------------------------===//

#include "AllocationOrder.h"
#include "LiveDebugVariables.h"
#include "RegAllocBase.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/CalcSpillWeights.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/Spiller.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <queue>
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/ScheduleDAGInstrs.h"
#include "llvm/CodeGen/SchedulePermuterHooks.h"
#include "ExpDAGPrinter.h"

using namespace llvm;


#define DEBUG_TYPE "regalloc"

static RegisterRegAlloc LRURegAlloc("LRU", "LRU register allocator",
                                      createLRURegisterAllocator);

std::string SchedOnlyFunc;
std::set<int> SchedOnlyBlock;


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

using MBBRegionsVector = SmallVector<std::pair<MachineBasicBlock*, SchedRegion>, 16>;

namespace {
class VregInterference : ScheduleDAGInstrs {
  public:
    static VregInterference* instance;

    static VregInterference* getInstance(MachineFunction* mf, const MachineLoopInfo *mli, AAResults* AA) {
      if(instance == nullptr) {
        instance = new VregInterference(mf, mli, AA);
      }
      return instance;
    }

    static VregInterference* getInstance() {
      return instance;
    }

    std::vector<MCRegister> getPreferredPhysRegs(unsigned vreg) {
      std::vector<MCRegister> prefs;
      auto it = compatibleVregs.find(vreg);
      if(it != compatibleVregs.end()) {
        for(auto& pref : it->second) {
          auto as =  assignments.find(pref);
          if(as != assignments.end())
            prefs.push_back(as->second);
        }
      }
      return prefs;
    }

    void assign(unsigned vreg, MCRegister phys) {
      assignments.insert({vreg, phys});
    }

    bool checkAssignmentHistory(unsigned vreg, MCRegister& phys) {
      //check if register has ever been assigned
      auto as =  assignments.find(vreg);
      if(as != assignments.end()) {
        phys = as->second;
        return true;
      }
      return false;
    }

    static bool usePrefs;

private:
  VregInterference(MachineFunction* mf, const MachineLoopInfo *mli, AAResults* AA) : ScheduleDAGInstrs(*mf, mli, false), mf(mf), AA(AA) {
      CalcTiedVregPrefs();
    };
  ~VregInterference() {};


  MBBRegionsVector Regions;
  void getSchedRegions(MachineBasicBlock *MBB, MBBRegionsVector &Regions);
  void CalcTiedVregPrefs();
  void getEdges(const SUnit *SU, std::vector<const SDep *> &Edges);
  void schedule() override {};
  // std::set<unsigned> targetOptVregs;

  MachineFunction* mf;
  AAResults* AA;
  std::set<unsigned> bbs;
  std::map<unsigned, std::set<unsigned>> compatibleVregs;
  std::map<unsigned, MCRegister> assignments;
};

VregInterference* VregInterference::instance = nullptr;
bool VregInterference::usePrefs = false;


void VregInterference::getEdges(const SUnit *SU, std::vector<const SDep *> &Edges) {
  for (const SDep &Pred : SU->Preds) {
    if (Pred.getKind() == SDep::Barrier || Pred.getKind() == SDep::Order) {
        Edges.push_back(&Pred);
    }
  }
}


void VregInterference::CalcTiedVregPrefs() {
  std::vector<std::pair<MachineInstr*, MachineInstr*>> edges;
  std::map<MachineInstr*, std::set<MachineInstr*>> iConnected;
  std::map<unsigned, std::set<MachineInstr*>> vregUsed;

  for(auto& MBB : MF) {
    getSchedRegions(&MBB, Regions);
  }

  int regionCounter = 0;
  for(auto& region : Regions) {
    regionCounter++;
    MachineBasicBlock* MBB = region.first;
    MachineBasicBlock::iterator I = region.second.RegionBegin;
    MachineBasicBlock::iterator RegionEnd = region.second.RegionEnd;
    unsigned NumRegionInstrs = region.second.NumRegionInstrs;
    startBlock(MBB);
    enterRegion(&*MBB, I, RegionEnd, NumRegionInstrs);

    // Skip empty scheduling regions (0 or 1 schedulable instructions).
    if (I == RegionEnd ) { //|| I == std::prev(RegionEnd)
      exitRegion();
      continue;
    }
    buildSchedGraph(AA);
    if(printScheduleDAGs) {
      std::string name = MBB->getFullName() + "_" + std::to_string(MBB->getNumber()) + "_" + std::to_string(regionCounter) + "PRE_RA";
      WriteExpGraph(this, name);
    }
    for(auto& SU : SUnits) {
      if(!SU.isInstr()) continue;
      MachineInstr* I = SU.getInstr();
      iConnected[I].insert(I);
      for(auto& operand : I->operands()) {
        if(operand.isReg() && operand.getReg().isVirtual()) {
          vregUsed[operand.getReg().virtRegIndex()].insert(I);
        }
      }
    }

    for(auto& SU : SUnits) {
      if(!SU.isInstr()) continue;
      std::vector<const SDep*> con;
      getEdges(&SU, con);
      for(auto& edge : con) {
        edges.push_back({SU.getInstr(), edge->getSUnit()->getInstr()});
      }
    }
    exitRegion();
  }

  bool change = true;
  while (change) {
    change = false;
    for(auto& edge : edges) {
      for(auto& i : iConnected) {
        if(i.second.find(edge.first) != i.second.end()) {
          auto res = i.second.insert(edge.second);
          change |= res.second;
        }
      }
    }
  }

  for(auto& vreg : vregUsed) {
    for(auto& vreg2 : vregUsed) {
      if(vreg.first == vreg2.first) 
        continue;
      bool compatible = true;
      for(auto& i : vreg.second) {
        for(auto& i2 : vreg2.second) {
          compatible &= (iConnected[i].find(i2) != iConnected[i].end() || iConnected[i2].find(i) != iConnected[i2].end());
        }        
      }
      if(compatible) {
        compatibleVregs[vreg.first].insert(vreg2.first);
        compatibleVregs[vreg2.first].insert(vreg.first);
      }
    }
  }

  // for(auto& a : compatibleVregs) {
  //     outs() << a.first << ": ";
  //     for(auto d : a.second) {
  //       outs() << d << " ";
  //     }
  //     outs() << "\n";
  //   }
}


void VregInterference::getSchedRegions(MachineBasicBlock *MBB, MBBRegionsVector &Regions) {
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
      Regions.push_back({MBB, SchedRegion(I, RegionEnd, NumRegionInstrs)});
  }

}
}

namespace {
  struct CompSpillWeight {
    bool operator()(const LiveInterval *A, const LiveInterval *B) const {
      return A->weight() < B->weight();
    }
  };
}

namespace {
/// RALRU provides a minimal implementation of the basic register allocation
/// algorithm. It prioritizes live virtual registers by spill weight and spills
/// whenever a register is unavailable. This is not practical in production but
/// provides a useful baseline both for measuring other allocators and comparing
/// the speed of the basic algorithm against other styles of allocators.
class RALRU : public MachineFunctionPass,
                public RegAllocBase,
                private LiveRangeEdit::Delegate {
  // context
  MachineFunction *MF;

  // state
  std::unique_ptr<Spiller> SpillerInstance;
  std::priority_queue<LiveInterval*, std::vector<LiveInterval*>,
                      CompSpillWeight> Queue;


  std::deque<MCRegister> LRU;
  std::set<MCRegister> seen;

  // Scratch space.  Allocated here to avoid repeated malloc calls in
  // selectOrSplit().
  BitVector UsableRegs;

  bool LRE_CanEraseVirtReg(Register) override;
  void LRE_WillShrinkVirtReg(Register) override;

public:
  RALRU(const RegClassFilterFunc F = allocateAllRegClasses);

  /// Return the pass name.
  StringRef getPassName() const override { return "LRU Register Allocator"; }

  /// RALRU analysis usage.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override;

  Spiller &spiller() override { return *SpillerInstance; }

  void enqueueImpl(LiveInterval *LI) override {
    Queue.push(LI);
  }

  LiveInterval *dequeue() override {
    if (Queue.empty())
      return nullptr;
    LiveInterval *LI = Queue.top();
    Queue.pop();
    return LI;
  }

  MCRegister selectOrSplit(LiveInterval &VirtReg,
                           SmallVectorImpl<Register> &SplitVRegs) override;

  /// Perform register allocation.
  bool runOnMachineFunction(MachineFunction &mf) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoPHIs);
  }

  MachineFunctionProperties getClearedProperties() const override {
    return MachineFunctionProperties().set(
      MachineFunctionProperties::Property::IsSSA);
  }

  // Helper for spilling all live virtual registers currently unified under preg
  // that interfere with the most recently queried lvr.  Return true if spilling
  // was successful, and append any new spilled/split intervals to splitLVRs.
  bool spillInterferences(LiveInterval &VirtReg, MCRegister PhysReg,
                          SmallVectorImpl<Register> &SplitVRegs);

  static char ID;
};

char RALRU::ID = 0;

} // end anonymous namespace

char &llvm::RALRUID = RALRU::ID;

INITIALIZE_PASS_BEGIN(RALRU, "regallocLRU", "LRU Register Allocator",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LiveDebugVariables)
INITIALIZE_PASS_DEPENDENCY(SlotIndexes)
INITIALIZE_PASS_DEPENDENCY(LiveIntervals)
INITIALIZE_PASS_DEPENDENCY(RegisterCoalescer)
INITIALIZE_PASS_DEPENDENCY(MachineScheduler)
INITIALIZE_PASS_DEPENDENCY(LiveStacks)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfo)
INITIALIZE_PASS_DEPENDENCY(VirtRegMap)
INITIALIZE_PASS_DEPENDENCY(LiveRegMatrix)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(RALRU, "regallocLRU", "LRU Register Allocator", false,
                    false)

bool RALRU::LRE_CanEraseVirtReg(Register VirtReg) {
  LiveInterval &LI = LIS->getInterval(VirtReg);
  if (VRM->hasPhys(VirtReg)) {
    Matrix->unassign(LI);
    aboutToRemoveInterval(LI);
    return true;
  }
  // Unassigned virtreg is probably in the priority queue.
  // RegAllocBase will erase it after dequeueing.
  // Nonetheless, clear the live-range so that the debug
  // dump will show the right state for that VirtReg.
  LI.clear();
  return false;
}

void RALRU::LRE_WillShrinkVirtReg(Register VirtReg) {
  if (!VRM->hasPhys(VirtReg))
    return;

  // Register is assigned, put it back on the queue for reassignment.
  LiveInterval &LI = LIS->getInterval(VirtReg);
  Matrix->unassign(LI);
  enqueue(&LI);
}

RALRU::RALRU(RegClassFilterFunc F):
  MachineFunctionPass(ID),
  RegAllocBase(F) {
}

void RALRU::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addPreserved<AAResultsWrapperPass>();
  AU.addRequired<LiveIntervals>();
  AU.addPreserved<LiveIntervals>();
  AU.addPreserved<SlotIndexes>();
  AU.addRequired<LiveDebugVariables>();
  AU.addPreserved<LiveDebugVariables>();
  AU.addRequired<LiveStacks>();
  AU.addPreserved<LiveStacks>();
  AU.addRequired<MachineBlockFrequencyInfo>();
  AU.addPreserved<MachineBlockFrequencyInfo>();
  AU.addRequiredID(MachineDominatorsID);
  AU.addPreservedID(MachineDominatorsID);
  AU.addRequired<MachineLoopInfo>();
  AU.addPreserved<MachineLoopInfo>();
  AU.addRequired<VirtRegMap>();
  AU.addPreserved<VirtRegMap>();
  AU.addRequired<LiveRegMatrix>();
  AU.addPreserved<LiveRegMatrix>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

void RALRU::releaseMemory() {
  SpillerInstance.reset();
}


// Spill or split all live virtual registers currently unified under PhysReg
// that interfere with VirtReg. The newly spilled or split live intervals are
// returned by appending them to SplitVRegs.
bool RALRU::spillInterferences(LiveInterval &VirtReg,
                                 MCRegister PhysReg,
                                 SmallVectorImpl<Register> &SplitVRegs) {
  // Record each interference and determine if all are spillable before mutating
  // either the union or live intervals.
  SmallVector<LiveInterval *, 8> Intfs;

  // Collect interferences assigned to any alias of the physical register.
  for (MCRegUnitIterator Units(PhysReg, TRI); Units.isValid(); ++Units) {
    LiveIntervalUnion::Query &Q = Matrix->query(VirtReg, *Units);
    for (auto *Intf : reverse(Q.interferingVRegs())) {
      if (!Intf->isSpillable() || Intf->weight() > VirtReg.weight())
        return false;
      Intfs.push_back(Intf);
    }
  }
  LLVM_DEBUG(dbgs() << "spilling " << printReg(PhysReg, TRI)
                    << " interferences with " << VirtReg << "\n");
  assert(!Intfs.empty() && "expected interference");

  // Spill each interfering vreg allocated to PhysReg or an alias.
  for (unsigned i = 0, e = Intfs.size(); i != e; ++i) {
    LiveInterval &Spill = *Intfs[i];

    // Skip duplicates.
    if (!VRM->hasPhys(Spill.reg()))
      continue;

    // Deallocate the interfering vreg by removing it from the union.
    // A LiveInterval instance may not be in a union during modification!
    Matrix->unassign(Spill);

    // Spill the extracted interval.
    LiveRangeEdit LRE(&Spill, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
    spiller().spill(LRE);
  }
  return true;
}


std::map<unsigned, MCRegister> tmp;
MCRegister RALRU::selectOrSplit(LiveInterval &VirtReg,
                                  SmallVectorImpl<Register> &SplitVRegs) {

    VregInterference* preferrence = VregInterference::getInstance();
    unsigned vregIndex = VirtReg.reg().virtRegIndex();
    //check history
    if(VregInterference::usePrefs) {
      MCRegister prev;
      bool done = preferrence->checkAssignmentHistory(vregIndex, prev);
      if(done) 
        return prev;
    }

  
    // Check for an available register in this class.
    auto Order = AllocationOrder::create(VirtReg.reg(), *VRM, RegClassInfo, Matrix);
    std::set<MCRegister> usable;
    for (MCRegister PhysReg : Order) { 
      assert(PhysReg.isValid());
      usable.insert(PhysReg);
      if(seen.find(PhysReg) == seen.end()) {
        seen.insert(PhysReg);
        LRU.push_front(PhysReg);
      }
    }

    //check if preferences are set
    if(VregInterference::usePrefs) {
      for(MCRegister reg : preferrence->getPreferredPhysRegs(vregIndex)) {
        if(usable.find(reg) == usable.end()) continue;
        if(Matrix->checkInterference(VirtReg, reg) == LiveRegMatrix::IK_Free) {
          //update preference assignments and LRU queue
          for(auto regIt = LRU.begin(); regIt != LRU.end(); regIt++) {
            if(*regIt == reg) {
              LRU.push_back(reg);
              LRU.erase(regIt);
            }
          }
          preferrence->assign(vregIndex, reg);
          tmp.insert({vregIndex, reg});
          return reg;
        }
      }
    }

    //no preference available fall back to LRU
    for(auto regIt = LRU.begin(); regIt != LRU.end(); regIt++) {
      llvm::MCRegister reg = *regIt;
      if(usable.find(reg) == usable.end()) continue;
      if(Matrix->checkInterference(VirtReg, reg) == LiveRegMatrix::IK_Free) {
        LRU.push_back(reg);
        LRU.erase(regIt);
        if(VregInterference::usePrefs) {
          preferrence->assign(vregIndex, reg);
          tmp.insert({vregIndex, reg});
        }
        return reg;
      }
    }

   
   //Spill
   if (!VirtReg.isSpillable())
     return ~0u;
   LiveRangeEdit LRE(&VirtReg, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
   spiller().spill(LRE);
  
   // The live virtual register requesting allocation was spilled, so tell
   // the caller not to allocate anything during this round.
   return 0;
}


bool RALRU::runOnMachineFunction(MachineFunction &mf) {
  LLVM_DEBUG(dbgs() << "********** LRU REGISTER ALLOCATION **********\n"
                    << "********** Function: " << mf.getName() << '\n');

  MF = &mf;
  RegAllocBase::init(getAnalysis<VirtRegMap>(),
                     getAnalysis<LiveIntervals>(),
                     getAnalysis<LiveRegMatrix>());
  VirtRegAuxInfo VRAI(*MF, *LIS, *VRM, getAnalysis<MachineLoopInfo>(),
                      getAnalysis<MachineBlockFrequencyInfo>());
  VRAI.calculateSpillWeightsAndHints();

  SpillerInstance.reset(createInlineSpiller(*this, *MF, *VRM, VRAI));


  if(mf.getName() == SchedOnlyFunc) {
    auto AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
    VregInterference::getInstance(MF, &getAnalysis<MachineLoopInfo>(), AA);
    VregInterference::usePrefs = true;
  }
  

  LRU.clear();
  seen.clear();

  allocatePhysRegs();
  postOptimization();

  // Diagnostic output before rewriting
  LLVM_DEBUG(dbgs() << "Post alloc VirtRegMap:\n" << *VRM << "\n");

  releaseMemory();
  VregInterference::usePrefs = false;
  return true;
}

FunctionPass* llvm::createLRURegisterAllocator() {
  return new RALRU();
}

FunctionPass* llvm::createLRURegisterAllocator(RegClassFilterFunc F) {
  return new RALRU(F);
}
