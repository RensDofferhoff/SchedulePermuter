//===----------------------------------------------------------------------===//
///
/// \file
///
/// This example demonstrates how to use LLJIT to:
///   - Add absolute symbol definitions.
///   - Run static constructors for a JITDylib.
///   - Run static destructors for a JITDylib.
///
/// This example does not call any functions (e.g. main or equivalent)  between
/// running the static constructors and running the static destructors.
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringMap.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"


#include "llvm/CodeGen/SchedulePermuterHooks.h"
#include "tree.h"
#include "init.h"
#include "utils.h"
#include "globals.h"



using namespace llvm;
using namespace llvm::orc;




void benchSchedulers(ThreadSafeModule& IRModule, std::vector<std::pair<std::string, std::vector<std::string>>>& schedulers) {
  bypassPreEmitScheduler = true;
  for(auto& sched : schedulers) {
    std::vector<char*> argv = createArgv(&sched.second);

    cl::ResetAllOptionOccurrences();
    cl::ParseCommandLineOptions(argv.size() - 1, argv.data(), "ScheduleAnalysis");
    auto J = ExitOnErr(LLJITBuilder().create());

    compile(J, IRModule, ExitOnErr);

    int (*func)() = (int(*)()) getFunctionPointer(J, SchedOnlyFunc, ExitOnErr);
    int (*prep)() = (int(*)()) getFunctionPointer(J, PrepFunc, ExitOnErr);
    prep();

    long long* results = benchmarkDefault(func);

    writeResultDefault(sched.first, currentHash, func(), results);
    delete results;

    cl::ResetAllOptionOccurrences();
    verify_clear = true;
  }
  bypassPreEmitScheduler = false;
}





int main(int argc, char *argv[]) {
  //init llvm needs to happen here
  InitLLVM X(argc, argv);
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  ExitOnErr.setBanner(std::string(argv[0]) + ": ");
  
  if(initAuxiliary(argc, argv)) {
    return -1;
  }

  std::vector<std::pair<std::string, std::vector<std::string>>> schedulers = {
  {"Default", {"-misched=default",  "-regalloc=LRU"}}, 
  {"No Post RA", {"-misched=default", "-regalloc=LRU", "-enable-post-misched=false", "-post-RA-scheduler=false"}}
  }; 
  benchSchedulers(globals.IRModule, schedulers);


  //create JIT
  std::vector<std::string>* arguments = new std::vector<std::string>({std::string(argv[0]), "-misched=default", "-enable-misched=false", "-enable-post-misched=false", "-regalloc=LRU", "-post-RA-scheduler=false"});
  arguments->insert(arguments->end(), JITarguments.begin(), JITarguments.end());
  std::vector<char*> argvLLVM = createArgv(arguments);
  cl::ParseCommandLineOptions(argvLLVM.size() - 1, argvLLVM.data(), "ScheduleAnalysis");
  auto J = ExitOnErr(LLJITBuilder().create());
  bypassPreEmitScheduler = false;

  linkScheduler();
  if(globals.viewBlocks) {
    outs() << "All machine basic blocks of the targeted function reaching the preemit scheduler:\n";
    printSchedulerMBBs = true;
    compile(J, globals.IRModule, ExitOnErr);
    getFunctionPointer(J, SchedOnlyFunc, ExitOnErr);
    getFunctionPointer(J, PrepFunc, ExitOnErr);
    return 0;
  }
  do {
    //compile
    compile(J, globals.IRModule, ExitOnErr);
    
    //prepare
    int (*func)() = (int(*)()) getFunctionPointer(J, SchedOnlyFunc, ExitOnErr);
    int (*prep)() = (int(*)()) getFunctionPointer(J, PrepFunc, ExitOnErr);
    prep();
    
    // bench
    long long* results = benchmarkDefault(func);

    // write results
    if(!verify_duplicate) {
      writeResultDefault("extern", currentHash, func(), results);
      if(verbose)
        outs() <<  "permutation: " << globals.defaultExp.permutation << "\n";
      globals.defaultExp.permutation++;
    }

    accumulateBenchDataCurrentNodeDefault((uint64_t*)results);
    delete [] results;
    
    
    //remove old binary
    ExitOnErr(J->getMainJITDylib().clear());

    //alter compilation parameters
    initNextRound();
  } while(!explorationFinished());
  outs() <<  "permutations: " << globals.defaultExp.permutation << "\n";
  cl::ResetAllOptionOccurrences();

  

  return 0;
}
