#ifndef SCHEDULE_LINK
#define SCHEDULE_LINK

#include "llvm/CodeGen/ScheduleDAG.h"
#include <set>

extern llvm::SUnit* (*pickNodeExtern) (bool &IsTopNode);
extern void (*releaseTopNodeExtern) (llvm::SUnit *SU);
extern void (*initExtern) ();
extern bool bypassPreEmitScheduler;
extern bool printSchedulerMBBs;
extern bool printScheduledMBBs;
extern bool printScheduleDAGs;
extern std::string hookOutputPath;

extern std::string SchedOnlyFunc;
extern std::string PrepFunc;
extern std::set<int> SchedOnlyBlock;
extern std::string currentHash;


extern bool verify_duplicate;
extern bool verify_clear;

#endif