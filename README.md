# pChop

A parallel implementation of Chopper(https://github.com/davidtr1037/chopper).

## Build

Process similar to Chopper 

* Build LLVM 3.4 using CMake
  ```
  mkdir build
  cd build
  cmake -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON <LLVM_SRC>
  make
  ```
* Build SVF (Pointer Analysis)
  https://github.com/davidtr1037/SVF/tree/master
* Build DG (Static Slicing)
  https://github.com/davidtr1037/dg/tree/master
* Build klee-uclibc
  https://github.com/davidtr1037/klee-uclibc
* STP Solver
  https://github.com/stp/stp
* Set up Open MPI 
  https://github.com/open-mpi/ompi 
* Build pChop
  ```
  git checkout master
  mkdir pchop_build
  cd pchop_build
  CXXFLAGS="-fno-rtti" cmake \
      -DENABLE_SOLVER_STP=ON \
      -DENABLE_POSIX_RUNTIME=ON \
      -DENABLE_KLEE_UCLIBC=ON \
      -DKLEE_UCLIBC_PATH=<KLEE_UCLIBC_DIR> \
      -DLLVM_CONFIG_BINARY=<LLVM_CONFIG_PATH> \
      -DENABLE_UNIT_TESTS=OFF \
      -DKLEE_RUNTIME_BUILD_TYPE=Release+Asserts \
      -DENABLE_SYSTEM_TESTS=ON \
      -DSVF_ROOT_DIR=<SVF_PROJECT_DIR> \
      -DDG_ROOT_DIR=<DG_PROJECT_DIR> \
      <PCHOP_ROOT_DIR>
  make
  ```
