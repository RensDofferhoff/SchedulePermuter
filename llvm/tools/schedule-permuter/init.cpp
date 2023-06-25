#include "init.h"
#include "globals.h"
#include "defaults.h"
#include "sched.h"
#include "getopt.h"
#include "llvm/CodeGen/SchedulePermuterHooks.h"
#include "stdlib.h"
#include "unistd.h"
#include "papi.h"
#include <sys/stat.h>

#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/Error.h"


#include "arrow/io/file.h"
#include "parquet/stream_writer.h"


using namespace llvm;
using namespace llvm::orc;

int pinCore(int core_id);
int initPAPI(std::string eventString, std::vector<std::string>& papiEventNames);
int initOutput(std::string path, std::vector<std::string>& papiEventNames);
inline Expected<ThreadSafeModule> parseModuleFromFile(StringRef FileName);
void printUsage();
int parseCommandlineArgs(int argc, char* argv[], std::string& IRPath, std::string& outPath, std::string& papiString);
int initTreeOutput(std::string path, std::vector<std::string>& papiEventNames);
int initOutputDir(std::string opath);
void parseBlocksList(std::string list);

Globals globals;
bool viewBlocks = false;
llvm::ExitOnError ExitOnErr;
std::vector<std::string> JITarguments;
bool verbose = false;
std::string PrepFunc;


int initAuxiliary(int argc, char *argv[]) {
    std::string IRpath, outPath, papiString;
    if(parseCommandlineArgs(argc, argv, IRpath, outPath, papiString)) {
        return -1;
    }

    pinCore(PINNED_CORE);

    //read file
    globals.IRModule = ExitOnErr(parseModuleFromFile(StringRef(IRpath)));
    
    //init PAPI 
    std::vector<std::string> papiEventsNames;
    if(initPAPI(papiString, papiEventsNames)) {
        outs() << "Could not setup papi\n";
        return -1;
    }
    
    
    if(initOutputDir(outPath) != 0) {
      outs() << "Could not create output directory\n";
      outs() << strerror(errno) << "\n";
      return -1;
    }
    if(*outPath.rbegin() != '/')
      outPath += "/" ;
    hookOutputPath = outPath;
    initOutput(outPath + "permutations", papiEventsNames);
    initTreeOutput(outPath + "tree", papiEventsNames);

    globals.defaultExp.numMeasurements = globals.defaultExp.numPapiEvents + 1; //+1 for time measurement
        
    return 0;
}


int pinCore(int core_id) {
  cpu_set_t my_set;
  CPU_ZERO(&my_set);
  CPU_SET(core_id, &my_set);
  sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
  return 1;
}


int initOutputDir(std::string opath) {
  struct stat stats;
  stat(opath.c_str(), &stats);
  if (S_ISDIR(stats.st_mode))
        return 0;
  return mkdir(opath.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
}

void parseBlocksList(std::string list) {
  std::string delimiter = ",";
    size_t pos = 0;
    int event;
    while ((pos = list.find(delimiter)) != std::string::npos) {
        event = std::stoi(list.substr(0, pos));
        SchedOnlyBlock.insert(event);
        list.erase(0, pos + delimiter.length());
    }
    if(list.size() != 0) {
      SchedOnlyBlock.insert(std::stoi(list));
    }
}

int initOutput(std::string path, std::vector<std::string>& papiEventNames) {
  std::shared_ptr<arrow::io::FileOutputStream> outfile;
  PARQUET_ASSIGN_OR_THROW(outfile, arrow::io::FileOutputStream::Open(path));
  
  parquet::WriterProperties::Builder builder;
  builder.compression(parquet::Compression::BROTLI);
  builder.encoding(parquet::Encoding::RLE);

  
  parquet::schema::NodeVector fields;

  fields.push_back(parquet::schema::PrimitiveNode::Make(
      "schedulerName", parquet::Repetition::REQUIRED, parquet::Type::BYTE_ARRAY,
      parquet::ConvertedType::UTF8));

  fields.push_back(parquet::schema::PrimitiveNode::Make(
        "permutation", parquet::Repetition::REQUIRED, parquet::Type::INT64,
        parquet::ConvertedType::UINT_64));

  fields.push_back(parquet::schema::PrimitiveNode::Make(
      "hash", parquet::Repetition::REQUIRED, parquet::Type::BYTE_ARRAY,
      parquet::ConvertedType::UTF8));

  fields.push_back(parquet::schema::PrimitiveNode::Make(
        "funcOut", parquet::Repetition::REQUIRED, parquet::Type::INT64,
        parquet::ConvertedType::UINT_64));

  for(auto& eventName : papiEventNames) {
    fields.push_back(parquet::schema::PrimitiveNode::Make(
        eventName, parquet::Repetition::REQUIRED, parquet::Type::INT64,
        parquet::ConvertedType::UINT_64));
  }

  fields.push_back(parquet::schema::PrimitiveNode::Make(
        "PAPI_TIME", parquet::Repetition::REQUIRED, parquet::Type::INT64,
        parquet::ConvertedType::UINT_64));

  auto schema  = std::static_pointer_cast<parquet::schema::GroupNode>(
      parquet::schema::GroupNode::Make("schema", parquet::Repetition::REQUIRED, fields));

  globals.dataOut = parquet::StreamWriter{parquet::ParquetFileWriter::Open(outfile, schema, builder.build())};
  return 0;
}

int initTreeOutput(std::string path, std::vector<std::string>& papiEventNames) {
  std::shared_ptr<arrow::io::FileOutputStream> outfile;
  PARQUET_ASSIGN_OR_THROW(outfile, arrow::io::FileOutputStream::Open(path));
  
  parquet::WriterProperties::Builder builder;
  builder.compression(parquet::Compression::BROTLI);
  builder.encoding(parquet::Encoding::RLE);

  
  parquet::schema::NodeVector fields;

  fields.push_back(parquet::schema::PrimitiveNode::Make(
      "NodeID", parquet::Repetition::REQUIRED, parquet::Type::INT64,
      parquet::ConvertedType::UINT_64));

  fields.push_back(parquet::schema::PrimitiveNode::Make(
        "Depth", parquet::Repetition::REQUIRED, parquet::Type::INT32,
        parquet::ConvertedType::UINT_32));

  fields.push_back(parquet::schema::PrimitiveNode::Make(
        "Parent", parquet::Repetition::REQUIRED, parquet::Type::INT64,
        parquet::ConvertedType::UINT_64));

  fields.push_back(parquet::schema::PrimitiveNode::Make(
        "EdgeIn", parquet::Repetition::REQUIRED, parquet::Type::BYTE_ARRAY,
        parquet::ConvertedType::UTF8));


  for(auto& eventName : papiEventNames) {
    fields.push_back(parquet::schema::PrimitiveNode::Make(
        eventName, parquet::Repetition::REQUIRED, parquet::Type::INT64,
        parquet::ConvertedType::UINT_64));
  }

  fields.push_back(parquet::schema::PrimitiveNode::Make(
        "PAPI_TIME", parquet::Repetition::REQUIRED, parquet::Type::INT64,
        parquet::ConvertedType::UINT_64));

  auto schema  = std::static_pointer_cast<parquet::schema::GroupNode>(
      parquet::schema::GroupNode::Make("schema", parquet::Repetition::REQUIRED, fields));

  globals.treeOut = parquet::StreamWriter{parquet::ParquetFileWriter::Open(outfile, schema, builder.build())};
  // globals.treeOut.SetMaxRowGroupSize(1024)
  return 0;

}




inline llvm::Error createSMDiagnosticError(llvm::SMDiagnostic &Diag) {
  using namespace llvm;
  std::string Msg;
  {
    raw_string_ostream OS(Msg);
    Diag.print("", OS);
  }
  return make_error<StringError>(std::move(Msg), inconvertibleErrorCode());
}


inline Expected<ThreadSafeModule> parseModuleFromFile(StringRef FileName) {
  auto Ctx = std::make_unique<LLVMContext>();
  SMDiagnostic Err;

  if (auto M = parseIRFile(FileName, Err, *Ctx))
    return ThreadSafeModule(std::move(M), std::move(Ctx));

  return createSMDiagnosticError(Err);
}


int initPAPI(std::string eventString, std::vector<std::string>& papiEventNames) {

    int papiRetval = PAPI_library_init(PAPI_VER_CURRENT);
    if(papiRetval != PAPI_VER_CURRENT) {
        errs() << "failed to init papi\n";
        return -1; 
    }

    globals.defaultExp.papiEventSet = PAPI_NULL;
    if(PAPI_create_eventset(&globals.defaultExp.papiEventSet) != PAPI_OK) {
        return -1;
    }

    std::string delimiter = ",";
    size_t pos = 0;
    std::string event;
    while ((pos = eventString.find(delimiter)) != std::string::npos) {
        event = eventString.substr(0, pos);
        papiEventNames.push_back(event);
        eventString.erase(0, pos + delimiter.length());
    }
    if(eventString.size() != 0) {
      papiEventNames.push_back(eventString);
    }

    int retval;
    for(auto& event : papiEventNames) {
        int eventCode;
        retval = PAPI_event_name_to_code(event.c_str(), &eventCode);
        if(retval != PAPI_OK) {
            errs() << "Could not encode PAPI event:" << event << "\n";
            return -1;
        }
        retval = PAPI_add_event(globals.defaultExp.papiEventSet, eventCode);
        if(retval != PAPI_OK) {
            errs() << "Could not add PAPI event, perhaps try with root or lower kernel/perf_event_paranoid\n";
            errs() << "Error: " << std::string(PAPI_strerror(retval)) << "\n";
            return -1;
        }
    }

    
    //alloc array to store future measurments of the evenset
    globals.defaultExp.numPapiEvents = papiEventNames.size();

    return 0;
}



void printUsage() {
    outs() << "Tool Usage:\n";
    outs() << "\t-view-mbb: Shows all the Machine Basic Blocks for the target function and quits\n";
    outs() << "\t-target-function-symbol: required function symbol target for the machine instruction schedulers\n";
    outs() << "\t-target-basic-block: optional specification of the target basic block of the function\n";
    outs() << "\t-prep-function-symbol: optional function symbol target that is run before a measurement to prepare/allocate initial data\n";
    outs() << "\t-IR-file: Necessary path to file containing compute and prepare symbol\n";
    outs() << "\t-output-dir: Necessary path to output directory\n";
    outs() << "\t-papi-event-list: , separated list of PAPI events to measure for each schedule\n";
    outs() << "\t-num-exp: Number of measurements per schedule\n";
    outs() << "\t-exp-iters: Number of iterations in a measurement\n";
    outs() << "\t-prep-iters: Number of iterations executed in preparation of an measurement\n";
    outs() << "\t-v: Verbose\n\n";
    outs() << "example:\nSchedulePermuter -target-function-symbol compute -IR-file test.ll -output-dir out -papi-event-list PAPI_TOT_CYC,PAPI_TLB_IM\n\n";

}


int parseCommandlineArgs(int argc, char* argv[], std::string& IRPath, std::string& outPath, std::string& papiString) {
  static option long_options[] =
        {
          {"view-mbb",         no_argument, 0, 'v'},
          {"target-function-symbol",         required_argument, 0, 'f'},
          {"prep-function-symbol",           required_argument, 0, 'u'},
          {"target-basic-block",             required_argument, 0, 'b'},
          {"exp-iters",         required_argument, 0, 'e'},
          {"num-exp",           required_argument, 0, 'n'},
          {"prep-iters",        required_argument, 0, 'p'},
          {"papi-event-list",   required_argument, 0, 'l'},
          {"IR-file",           required_argument, 0, 'i'},
          {"output-dir",       required_argument, 0, 'o'},
          {"randomDF",          no_argument, 0, 'r'},
          {"v",                 no_argument, 0, 'y'},
          {"help",              no_argument, 0, 'h'},
          {0, 0, 0, 0}
        };

  int opt;
  bool inSet = false;
  bool outSet = false;
  bool funcset = false;
  while ((opt = getopt_long_only(argc, argv, "", long_options, NULL)) != -1) {
    switch (opt) {
          case 'v':
              globals.viewBlocks = true;
              break;
         case 'f':
              SchedOnlyFunc = std::string(optarg);
              funcset = true;
              break;
         case 'u':
              PrepFunc = std::string(optarg);
              break;
         case 'y':
              verbose = true;
              break;
         case 'b':
              parseBlocksList(std::string(optarg));
              break;
         case 'e':
              globals.defaultExp.expIters = atoi(optarg);
              break;
         case 'n':
              globals.defaultExp.numExpPerSchedule = atoi(optarg);
              break;
        case 'p':
              globals.defaultExp.prepIters = atoi(optarg);
              break;
        case 'r':
              globals.defaultExp.selectionMode = SelectionMode::select_random;
              break;
        case 'i':
              IRPath = std::string(optarg);
              inSet = true;
              break;
        case 'o':
              outPath = std::string(optarg);
              outSet = true;
              break;
        case 'l':
              papiString = std::string(optarg);
              break;
        case 'h':
              printUsage();
              return -1;
        case '?':
              printUsage();
              return -1;

      }
    }
    if(!(inSet && outSet && funcset)){
      printUsage();
      return -1; 
    }
    return 0;
  
}