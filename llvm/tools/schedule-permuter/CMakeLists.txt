add_compile_options(-fexceptions)
set(LLVM_LINK_COMPONENTS
  Core
  ExecutionEngine
  IRReader
  JITLink
  OrcJIT
  Support
  CodeGen
  nativecodegen
  TransformUtils
  SelectionDAG
  )



add_llvm_tool(SchedulePermuter
  SchedulePermuter.cpp
  init.cpp
  utils.cpp
  tree.cpp
  )



find_library(ArrowLib arrow)
find_library(ParquetLib parquet)
# find_path (ArrowInclude arrow)
# find_path (ParquetInclude parquet)

# target_include_directories(SchedulePermuter "${ArrowInclude}" "${ParquetInclude}")

target_link_libraries(SchedulePermuter PRIVATE "${ArrowLib}" "${ParquetLib}")
target_link_libraries(SchedulePermuter PRIVATE -lpapi)
