#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/InitializePasses.h"
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "llvm/Support/SHA256.h"
#include "llvm/CodeGen/SchedulePermuterHooks.h"


#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetInstrInfo.h"



using namespace llvm;
          
#define MACHINEVERF_NAME "Schedule verification & duplicate detector"

bool verify_duplicate;
bool verify_clear;
std::string currentHash;

namespace {
using opcode = unsigned;

template <typename T>
void
print_hash(char* buf, const T& hash)
{
    for (auto c : hash) {
        buf += sprintf(buf, "%02x", c);
    }
}

class VerfData {

protected:
    VerfData(){};
    static VerfData* verfdata;

public:
    static VerfData* getVerfData() {if(verfdata) return verfdata; else verfdata = new VerfData(); return verfdata;};
    std::map<std::string, std::map<opcode, unsigned>> instrCounts;
    std::unordered_set<std::string> hashes;
};
VerfData* VerfData::verfdata = nullptr;

class MachineInstrScheduleVerf: public MachineFunctionPass {
public: 
    static char ID;
    
    
    MachineInstrScheduleVerf() : MachineFunctionPass(ID) {
        initializeMachineInstrScheduleVerfPass(*PassRegistry::getPassRegistry());
        data = VerfData::getVerfData();
        if(verify_clear) {
            data->hashes.clear();
            data->instrCounts.clear();
            verify_clear = false;
        }
    }

    ~MachineInstrScheduleVerf() {
    }
    
          
    bool runOnMachineFunction(MachineFunction &MF) override;
    StringRef getPassName() const override { return MACHINEVERF_NAME; };
private:
    VerfData* data;
};

char MachineInstrScheduleVerf::ID = 0;
        
bool MachineInstrScheduleVerf::runOnMachineFunction(MachineFunction &MF) {
    if(MF.getName() != SchedOnlyFunc) return false;

    std::string schedule;
    llvm::raw_string_ostream ss(schedule);
    //count the various instructions
    std::map<opcode, unsigned> counts;
    for (MachineBasicBlock &MBB : MF) {
        for (auto &MI : MBB) {
            if (SchedOnlyBlock.find(MBB.getNumber()) == SchedOnlyBlock.end())
      		continue;
            counts[MI.getOpcode()] += 1;
            MI.print(ss);
        }
    }
    

    //set or load previous counts
    auto prevCountsIt = data->instrCounts.find(std::string(MF.getName()));
    if(prevCountsIt == data->instrCounts.end()) {
       data->instrCounts[std::string(MF.getName())] = counts;
       prevCountsIt = data->instrCounts.find(std::string(MF.getName()));
    }

    // auto TTI = MF.getSubtarget().getInstrInfo();
    // for(auto& instr : counts) {
    //     outs() << TTI->getName(instr.first) << ": " << instr.second << "\n";
    // }

    //compare counts
    auto& prevCounts = prevCountsIt->second;
    std::vector<opcode> instrs;
    for(auto& count : prevCounts)
        instrs.push_back(count.first);
    for(auto& count : counts) {
        instrs.push_back(count.first);
    }


        
    //check if the same instructions are present
    for(auto instr : instrs) {
        auto current = counts.find(instr);
        auto prev = prevCounts.find(instr);
        if(prev == prevCounts.end() || current == counts.end() || prev->second != current->second) {
            errs() << "instructions do not match\n";
            abort();
        }

        // assert((countsIt != counts.end() && countsIt->second == count.second) && "Schedule does not contain the same instructions!");
    }


    //check to see if we have seen this permutation in the past
    auto hash = SHA256::hash(ArrayRef<uint8_t>((const uint8_t*)schedule.c_str(), schedule.length()));
    char hashStr[400] = {0};
    print_hash(hashStr, hash);
    
    currentHash = std::string(hashStr);
    auto res = data->hashes.insert(currentHash);
    verify_duplicate = !res.second;
    return false;
}        
} // end of anonymous namespace

FunctionPass *llvm::createMachineInstrScheduleVerfPass() { return new MachineInstrScheduleVerf(); }
        
INITIALIZE_PASS(MachineInstrScheduleVerf, "machineinstr-verf", MACHINEVERF_NAME, true, true)
                   

