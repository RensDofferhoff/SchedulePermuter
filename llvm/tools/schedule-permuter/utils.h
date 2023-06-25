#ifndef UTILS
#define UTILS

#include <vector>
#include <string>
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "parquet/stream_writer.h"

class ExpParams;

std::vector<char*> createArgv(std::vector<std::string>* arguments);
void compile(std::unique_ptr<llvm::orc::LLJIT>& J, llvm::orc::ThreadSafeModule& IRModule, llvm::ExitOnError& ExitOnErr);
void* getFunctionPointer(std::unique_ptr<llvm::orc::LLJIT>& J, std::string functionName, llvm::ExitOnError& ExitOnErr);
long long* benchmark(int (*func)(), ExpParams params);
long long* benchmarkDefault(int (*func)());
int writeResultDefault(std::string name, std::string hash, long long funcOutput, long long* results);
int writeResult(parquet::StreamWriter& out, std::string name, std::string hash, long long funcOutput, long long* results, ExpParams params);

#endif