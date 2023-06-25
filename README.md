# SchedulePermuter based on LLVM

Generates and benchmarks all valid permutations of the provided compute kernel.

# Compilation

Our data collection tool named SchedulePermuter is based on LLVM 14.0.5. Its only dependencies
are Apache Parquet, cmake and ninja. Most package managers provide packages for these
dependencies. To compile our tool on Unix-like operating systems for the X86 architecture clone this repo 
and run the following commands from the top of the folder:

```
mkdir llvm/build
cd llvm/build/
cmake -DCMAKE_BUILD_TYPE=Debug -DLLVM_TARGETS_TO_BUILD=X86 \
-DBUILD_SHARED_LIBS=ON ../ -GNinja
ninja SchedulePermuter
```

The SchedulePermuter binary can now be found in build/bin/.

# Usage 
For usage details see thesis
