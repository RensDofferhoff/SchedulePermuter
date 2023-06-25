#include "utils.h"
#include "papi.h"
#include "globals.h"

#include <vector>
#include <string>

#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "parquet/stream_writer.h"
#include "llvm/Support/raw_ostream.h"

std::vector<char*> createArgv(std::vector<std::string>* arguments) {
    std::vector<char*> argv;
    for (const auto& arg : *arguments)
        argv.push_back((char*)arg.data());
    argv.push_back(nullptr);
    return argv;
}

void compile(std::unique_ptr<llvm::orc::LLJIT>& J, llvm::orc::ThreadSafeModule& IRModule, llvm::ExitOnError& ExitOnErr) {
    auto M = llvm::orc::ThreadSafeModule(llvm::CloneModule(*IRModule.getModuleUnlocked()), IRModule.getContext());
    ExitOnErr(J->addIRModule(std::move(M)));
}

void* getFunctionPointer(std::unique_ptr<llvm::orc::LLJIT>& J, std::string functionName, llvm::ExitOnError& ExitOnErr) {
    auto funcSym = ExitOnErr(J->lookup(functionName.c_str()));
    return (void*) funcSym.getAddress();
}

// void* getFunctionPointer(std::unique_ptr<llvm::orc::LLJIT>& J, std::string functionName) {
//     auto funcSym = J->lookup(functionName.c_str());
//     return (void*) funcSym.getAddress();
// }

long long* benchmark(int (*func)(), ExpParams params) {
    int numMeasurPerExp = globals.defaultExp.numMeasurements;
    long long* results = new long long[params.numExpPerSchedule * numMeasurPerExp]; 
    long long s,e;

    for(int exp = 0; exp < params.numExpPerSchedule; exp++) {

        for(int i = 0; i < params.prepIters; i++) {
            func();
        }

        s = PAPI_get_real_nsec();

        if (PAPI_start(params.papiEventSet) != PAPI_OK){
            return nullptr;
        }

        for(int i = 0; i < params.expIters; i++) {
            func();
        }

        if (PAPI_stop(params.papiEventSet, results + exp * numMeasurPerExp) != PAPI_OK){
            return nullptr;
        }

        e = PAPI_get_real_nsec();
        results[exp * numMeasurPerExp + (numMeasurPerExp - 1)] = e - s;

    }
    return results;
}


long long* benchmarkDefault(int (*func)()) {
    return benchmark(func, globals.defaultExp);
}


int writeResult(parquet::StreamWriter& out, std::string name, std::string hash, long long funcOutput, long long* results, ExpParams params) {
    int numMeasurPerExp = globals.defaultExp.numMeasurements;
    for(int exp = 0; exp < params.numExpPerSchedule; exp++) {
        out << name;
        out << (uint64_t)params.permutation;
        out << hash;
        out << (uint64_t)funcOutput;
        for(int event = 0; event < numMeasurPerExp; event++) {
            out << (uint64_t) results[exp * numMeasurPerExp + event];//weird typing system stuff is weird
        }
        // out << (uint64_t)12;
        out << parquet::EndRow;
    }
    return 0;
}

int writeResultDefault(std::string name, std::string hash, long long funcOutput, long long* results) {
    return writeResult(globals.dataOut, name, hash, funcOutput, results,  globals.defaultExp);
}

