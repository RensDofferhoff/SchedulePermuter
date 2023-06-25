#ifndef SCHEDULE_ANALYSIS_GLOBALS
#define SCHEDULE_ANALYSIS_GLOBALS

#include "defaults.h"
#include <string>
#include <vector>
#include "llvm/Support/Error.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "parquet/stream_writer.h"


enum SelectionMode {select_first = 0, select_random = 1};


//experimental params
struct ExpParams
{
    int numExpPerSchedule{NUM_EXP};
    int prepIters{PREP_ITERS};
    int expIters{EXP_ITERS};
    int papiEventSet;
    int numPapiEvents;
    int numMeasurements;
    long long permutation{0};
    int selectionMode{SelectionMode::select_first};
};


struct Globals
{
    ExpParams defaultExp;
    llvm::orc::ThreadSafeModule IRModule;
    parquet::StreamWriter dataOut;
    parquet::StreamWriter treeOut;
    bool viewBlocks{false};
};

extern Globals globals;
extern llvm::ExitOnError ExitOnErr;
extern std::vector<std::string> JITarguments;
extern bool verbose;

#endif